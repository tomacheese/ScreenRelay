#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
// PKEY_Device_FriendlyName の実体を確保するため INITGUID を定義してから
// functiondiscoverykeys_devpkey.h をインクルードする (MinGW では propsys.lib が
// このシンボルの実体を提供しないため)。
#define INITGUID
#include <functiondiscoverykeys_devpkey.h>
#undef INITGUID
#include <propvarutil.h>

#include "audio/audio_capture.hpp"
#include "common/time_utils.hpp"
#include <chrono>
#include <cstring>

/**
 * @brief COM インターフェースポインタを解放するヘルパー
 */
template <typename T>
static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

/**
 * @brief wstring を UTF-8 std::string に変換する
 */
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

/**
 * @brief IMMDevice のフレンドリ名を取得する
 */
static std::string get_friendly_name(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return {};

    PROPVARIANT var;
    PropVariantInit(&var);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
        name = wide_to_utf8(var.pwszVal);
    }
    PropVariantClear(&var);
    safe_release(store);
    return name;
}

/**
 * @brief WasapiAudioCapture の内部実装
 *
 * COM インターフェースと WAVEFORMATEX を保持する。
 */
struct WasapiAudioCapture::Impl {
    IMMDeviceEnumerator* enumerator      = nullptr;  ///< デバイス列挙用
    IMMDevice*           device          = nullptr;  ///< 対象デバイス
    IAudioClient*         audio_client   = nullptr;  ///< オーディオクライアント
    IAudioCaptureClient*  capture_client = nullptr;  ///< キャプチャクライアント
    WAVEFORMATEX*         mix_format     = nullptr;  ///< デバイスのミックスフォーマット
    HANDLE                event_handle   = nullptr;  ///< バッファ到着通知イベント
    bool                  com_initialized_here = false;  ///< このスレッドで CoInitializeEx を呼んだか

    /** @brief すべての COM リソースを解放する */
    void release_all() {
        if (audio_client) audio_client->Stop();
        if (mix_format)  { CoTaskMemFree(mix_format); mix_format = nullptr; }
        safe_release(capture_client);
        safe_release(audio_client);
        safe_release(device);
        safe_release(enumerator);
        if (event_handle) { CloseHandle(event_handle); event_handle = nullptr; }
    }
};

WasapiAudioCapture::WasapiAudioCapture() : impl_(std::make_unique<Impl>()) {}

WasapiAudioCapture::~WasapiAudioCapture() { release(); }

std::vector<AudioDeviceInfo> WasapiAudioCapture::enumerate_devices(std::string& error) {
    std::vector<AudioDeviceInfo> result;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool initialized_here = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        error = "CoCreateInstance(MMDeviceEnumerator) failed";
        if (initialized_here) CoUninitialize();
        return result;
    }

    // eRender（ループバック用）と eCapture（録音デバイス）の両方を列挙する
    const EDataFlow flows[2] = { eRender, eCapture };
    for (EDataFlow flow : flows) {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr) || !collection) continue;

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev)) || !dev) continue;

            LPWSTR id_wstr = nullptr;
            if (SUCCEEDED(dev->GetId(&id_wstr)) && id_wstr) {
                AudioDeviceInfo info;
                info.id        = wide_to_utf8(id_wstr);
                info.name      = get_friendly_name(dev);
                info.is_render = (flow == eRender);
                result.push_back(std::move(info));
                CoTaskMemFree(id_wstr);
            }
            safe_release(dev);
        }
        safe_release(collection);
    }

    safe_release(enumerator);
    if (initialized_here) CoUninitialize();
    return result;
}

bool WasapiAudioCapture::init(const AudioConfig& config, std::string& error) {
    release();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE はこのスレッドで既に別モードの COM が初期化済みという
    // ことであり、致命的ではないため続行する。
    impl_->com_initialized_here = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&impl_->enumerator));
    if (FAILED(hr) || !impl_->enumerator) {
        error = "CoCreateInstance(MMDeviceEnumerator) failed";
        return false;
    }

    AudioSourceKind source_kind = AudioSourceKind::Loopback;
    if (!parse_audio_source_kind(config.source, source_kind)) {
        error = "Invalid audio.source: " + config.source;
        return false;
    }
    const bool is_loopback = (source_kind == AudioSourceKind::Loopback);
    const EDataFlow flow   = is_loopback ? eRender : eCapture;

    if (!config.device_id.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, config.device_id.c_str(), -1, nullptr, 0);
        std::wstring wid(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, config.device_id.c_str(), -1, wid.data(), wlen);
        hr = impl_->enumerator->GetDevice(wid.c_str(), &impl_->device);
        if (FAILED(hr) || !impl_->device) {
            error = "Audio device not found: " + config.device_id;
            return false;
        }
    } else {
        hr = impl_->enumerator->GetDefaultAudioEndpoint(flow, eConsole, &impl_->device);
        if (FAILED(hr) || !impl_->device) {
            error = "GetDefaultAudioEndpoint failed (no default "
                    + std::string(is_loopback ? "render" : "capture") + " device)";
            return false;
        }
    }

    hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                 nullptr, reinterpret_cast<void**>(&impl_->audio_client));
    if (FAILED(hr) || !impl_->audio_client) {
        error = "IMMDevice::Activate(IAudioClient) failed";
        return false;
    }

    hr = impl_->audio_client->GetMixFormat(&impl_->mix_format);
    if (FAILED(hr) || !impl_->mix_format) {
        error = "IAudioClient::GetMixFormat failed";
        return false;
    }

    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (is_loopback) stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    // バッファ長は 200ms 相当。イベント駆動のため実際の遅延はイベント間隔に依存する。
    constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;  // 100ns 単位
    hr = impl_->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                        kBufferDuration, 0, impl_->mix_format, nullptr);
    if (FAILED(hr)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        error = "IAudioClient::Initialize failed: " + std::string(buf);
        return false;
    }

    impl_->event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event_handle) {
        error = "CreateEventW failed";
        return false;
    }
    hr = impl_->audio_client->SetEventHandle(impl_->event_handle);
    if (FAILED(hr)) {
        error = "IAudioClient::SetEventHandle failed";
        return false;
    }

    hr = impl_->audio_client->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(&impl_->capture_client));
    if (FAILED(hr) || !impl_->capture_client) {
        error = "IAudioClient::GetService(IAudioCaptureClient) failed";
        return false;
    }

    sample_rate_     = static_cast<int>(impl_->mix_format->nSamplesPerSec);
    channels_        = impl_->mix_format->nChannels;
    bits_per_sample_ = impl_->mix_format->wBitsPerSample;
    is_float_        = (impl_->mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (impl_->mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(impl_->mix_format);
        is_float_ = (ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
    }

    hr = impl_->audio_client->Start();
    if (FAILED(hr)) {
        error = "IAudioClient::Start failed";
        return false;
    }

    hard_error_.store(false);
    running_.store(true);
    thread_ = std::thread(&WasapiAudioCapture::capture_thread_func, this);
    return true;
}

void WasapiAudioCapture::capture_thread_func() {
    while (running_.load()) {
        DWORD wait_result = WaitForSingleObject(impl_->event_handle, 2000);
        if (!running_.load()) break;

        if (wait_result != WAIT_OBJECT_0) {
            // 2 秒間バッファが届かない場合はデバイス切断等の可能性がある。
            // ハードエラーとして通知し、呼び出し元の再初期化に委ねる。
            {
                std::lock_guard<std::mutex> lk(last_error_mutex_);
                last_error_ = "Audio capture event wait timed out";
            }
            // ハードエラー確定後もループを回し続けると同じ失敗を延々と繰り返すため、
            // ここでスレッド自体を終了させ、呼び出し側の再初期化に委ねる。
            hard_error_.store(true);
            running_.store(false);
            break;
        }

        UINT32 packet_frames = 0;
        HRESULT hr = impl_->capture_client->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "GetNextPacketSize failed";
            hard_error_.store(true);
            running_.store(false);
            break;
        }

        while (packet_frames > 0) {
            BYTE* data      = nullptr;
            UINT32 num_frames = 0;
            DWORD  flags      = 0;

            hr = impl_->capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            AudioBuffer buf;
            buf.frame_count = num_frames;
            const size_t byte_count =
                static_cast<size_t>(num_frames) * impl_->mix_format->nBlockAlign;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // 無音期間はゼロ埋めのバッファを生成する（ループバック時、無音でも
                // フレームは配信し続けないと受信側の音声タイムラインが途切れる）。
                buf.data.assign(byte_count, 0);
            } else if (data && byte_count > 0) {
                buf.data.assign(data, data + byte_count);
            }

            impl_->capture_client->ReleaseBuffer(num_frames);

            if (!buf.data.empty() || num_frames > 0) {
                AudioMeta meta;
                meta.timestamp_us = time_utils::system_now_us();

                std::lock_guard<std::mutex> lk(mutex_);
                queue_.emplace(std::move(buf), meta);
                while (queue_.size() > static_cast<size_t>(kMaxQueueSize)) queue_.pop();
                cv_.notify_one();
            }

            hr = impl_->capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) break;
        }
    }
}

bool WasapiAudioCapture::try_pop(AudioBuffer& buf, AudioMeta& meta) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) return false;
    buf  = std::move(queue_.front().first);
    meta = queue_.front().second;
    queue_.pop();
    return true;
}

bool WasapiAudioCapture::wait_pop(AudioBuffer& buf, AudioMeta& meta, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty(); })) {
        return false;
    }
    buf  = std::move(queue_.front().first);
    meta = queue_.front().second;
    queue_.pop();
    return true;
}

void WasapiAudioCapture::release() {
    running_.store(false);
    // capture_thread_func のイベント待ちを解除して即座に終了させる
    if (impl_->event_handle) SetEvent(impl_->event_handle);
    if (thread_.joinable()) thread_.join();

    impl_->release_all();
    if (impl_->com_initialized_here) {
        CoUninitialize();
        impl_->com_initialized_here = false;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    std::queue<std::pair<AudioBuffer, AudioMeta>> empty;
    std::swap(queue_, empty);
}

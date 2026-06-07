#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "capture/dxgi_capture.hpp"
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Impl 定義
// ---------------------------------------------------------------------------

/**
 * @brief DxgiCaptureBackend の実装詳細を保持する構造体
 *
 * Pimpl イディオムにより、D3D11/DXGI の型をヘッダーに露出させない。
 */
struct DxgiCaptureBackend::Impl {
    ID3D11Device*             device         = nullptr;  ///< D3D11 デバイス
    ID3D11DeviceContext*      context        = nullptr;  ///< D3D11 デバイスコンテキスト
    IDXGIOutputDuplication*   duplication    = nullptr;  ///< デスクトップ複製インターフェース
    ID3D11Texture2D*          staging_tex    = nullptr;  ///< CPU 読み取り用ステージングテクスチャ
    SwsContext*               sws_ctx        = nullptr;  ///< libswscale スケーリングコンテキスト
    int     physical_width  = 0;   ///< 物理解像度の幅
    int     physical_height = 0;   ///< 物理解像度の高さ
    uint64_t frame_seq      = 0;   ///< フレームシーケンス番号

    /**
     * @brief GPU ゼロコピーパス用テクスチャプール
     *
     * AcquireNextFrame で取得したテクスチャは ReleaseFrame() 後に無効になるため、
     * CPU を介さず GPU 上でこのプール内のテクスチャへ CopyResource する。
     * リングバッファ方式で使い回すことで、フリーズフレームキャッシュや
     * フレームキューに残った古い参照の内容が新しいキャプチャで上書きされる
     * ことを防ぐ（D3D11 イミディエイトコンテキストはコマンドの発行順序を
     * 保証するため、サブリソースの読み取り後書き込みの整合性は保たれる）。
     */
    static constexpr int kZeroCopyPoolSize = 6;
    ID3D11Texture2D* zero_copy_pool[kZeroCopyPoolSize] = {};  ///< プール本体
    int  zero_copy_pool_idx = 0;                              ///< 次に使用するプールインデックス
};

// ---------------------------------------------------------------------------
// コンストラクタ / デストラクタ
// ---------------------------------------------------------------------------

/**
 * @brief コンストラクタ
 */
DxgiCaptureBackend::DxgiCaptureBackend()
    : impl_(std::make_unique<Impl>()) {}

/**
 * @brief デストラクタ。release() を呼び出してリソースを解放する
 */
DxgiCaptureBackend::~DxgiCaptureBackend() {
    release();
}

// ---------------------------------------------------------------------------
// ICaptureBackend の実装
// ---------------------------------------------------------------------------

/**
 * @brief 指定モニターに対してキャプチャを初期化する
 * @param monitor          対象 HMONITOR
 * @param logical_width    論理解像度の幅
 * @param logical_height   論理解像度の高さ
 * @return 成功した場合 true
 */
bool DxgiCaptureBackend::init(HMONITOR monitor, int logical_width, int logical_height) {
    // 再初期化時はハードエラーフラグをクリアする
    hard_error_.store(false);
    logical_width_  = logical_width;
    logical_height_ = logical_height;

    if (!find_and_init(monitor)) {
        return false;
    }

    // 物理解像度と論理解像度が異なる場合のみ sws_ctx を作成する
    if (impl_->physical_width != logical_width || impl_->physical_height != logical_height) {
        impl_->sws_ctx = sws_getContext(
            impl_->physical_width,  impl_->physical_height, AV_PIX_FMT_BGRA,
            logical_width,          logical_height,          AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!impl_->sws_ctx) {
            last_error_ = "sws_getContext failed";
            release();
            return false;
        }
    }

    return true;
}

/**
 * @brief 最新フレームを取得する
 *
 * AcquireNextFrame → ステージングテクスチャへコピー → Map → FrameBuffer に格納する。
 * 物理解像度と論理解像度が異なる場合は sws_scale でダウンスケールする。
 *
 * @param timeout_ms   タイムアウト (ms)
 * @return フレームバッファ。タイムアウト時は nullopt
 */
std::optional<FrameBuffer> DxgiCaptureBackend::acquire_frame(int timeout_ms) {
    if (!impl_->duplication) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "Not initialized";
        }
        return std::nullopt;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    IDXGIResource*           desktop_resource = nullptr;

    // 次のフレームを取得する
    HRESULT hr = impl_->duplication->AcquireNextFrame(
        static_cast<UINT>(timeout_ms),
        &frame_info,
        &desktop_resource
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // タイムアウトは正常（画面に変化なし）。フレームなしで返す
        return std::nullopt;
    }

    if (FAILED(hr)) {
        // ACCESS_LOST 等の回復不能エラー。
        // last_error_ を先に書き込んでから atomic フラグを立てることで
        // コンシューマーがフラグを読んだ際に文字列が確実に可視となる。
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "AcquireNextFrame failed: HRESULT=" + std::to_string(static_cast<long>(hr));
        }
        hard_error_.store(true);
        return std::nullopt;
    }

    // IDXGIResource から ID3D11Texture2D を取得する
    ID3D11Texture2D* tex = nullptr;
    hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    desktop_resource->Release();
    desktop_resource = nullptr;

    if (FAILED(hr) || !tex) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "QueryInterface for ID3D11Texture2D failed";
        }
        impl_->duplication->ReleaseFrame();
        return std::nullopt;
    }

    // GPU ゼロコピーモード: CPU を介さず GPU 上でテクスチャプールへコピーし、
    // 参照をそのまま FrameBuffer に格納して返す。Map/memcpy/sws_scale を行わない。
    if (zero_copy_mode_.load()) {
        ID3D11Texture2D* dst = impl_->zero_copy_pool[impl_->zero_copy_pool_idx];
        impl_->zero_copy_pool_idx = (impl_->zero_copy_pool_idx + 1) % Impl::kZeroCopyPoolSize;

        // GPU 内でのテクスチャ間コピー (CPU 同期を伴わない)
        impl_->context->CopyResource(dst, tex);
        tex->Release();
        tex = nullptr;
        impl_->duplication->ReleaseFrame();

        // 参照カウントを増やしてラップする。デリーターが Release() を呼び出すことで
        // テクスチャはプールに留まり続け、FrameBuffer 側は単に参照を保持するのみとなる。
        dst->AddRef();
        std::shared_ptr<void> handle(dst, [](void* p) {
            static_cast<ID3D11Texture2D*>(p)->Release();
        });

        FrameBuffer fb;
        fb.width       = static_cast<uint32_t>(impl_->physical_width);
        fb.height      = static_cast<uint32_t>(impl_->physical_height);
        fb.format      = PixelFormat::BGRA;
        fb.gpu_texture = std::move(handle);
        return fb;
    }

    // ステージングテクスチャへコピーする
    impl_->context->CopyResource(impl_->staging_tex, tex);
    tex->Release();
    tex = nullptr;

    // Map して CPU からデータを読み取る
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = impl_->context->Map(impl_->staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "ID3D11DeviceContext::Map failed";
        }
        impl_->duplication->ReleaseFrame();
        return std::nullopt;
    }

    const int phys_w = impl_->physical_width;
    const int phys_h = impl_->physical_height;

    if (impl_->sws_ctx) {
        // 物理解像度 → 論理解像度へスケールダウンする
        FrameBuffer fb;
        fb.width  = static_cast<uint32_t>(logical_width_);
        fb.height = static_cast<uint32_t>(logical_height_);
        fb.format = PixelFormat::BGRA;
        fb.data.resize(static_cast<size_t>(logical_width_) * static_cast<size_t>(logical_height_) * 4u);

        // sws_scale に渡すためのポインタ配列を構築する
        const uint8_t* src_data[1] = { static_cast<const uint8_t*>(mapped.pData) };
        int            src_stride[1] = { static_cast<int>(mapped.RowPitch) };

        uint8_t* dst_data[1]  = { fb.data.data() };
        int      dst_stride[1] = { logical_width_ * 4 };

        sws_scale(
            impl_->sws_ctx,
            src_data,   src_stride, 0, phys_h,
            dst_data,   dst_stride
        );

        impl_->context->Unmap(impl_->staging_tex, 0);
        impl_->duplication->ReleaseFrame();

        return fb;
    } else {
        // スケーリング不要。BGRA データをそのままコピーする
        FrameBuffer fb;
        fb.width  = static_cast<uint32_t>(phys_w);
        fb.height = static_cast<uint32_t>(phys_h);
        fb.format = PixelFormat::BGRA;
        fb.data.resize(static_cast<size_t>(phys_w) * static_cast<size_t>(phys_h) * 4u);

        // 行ピッチが幅×4 と一致しない場合は行単位でコピーする
        const size_t row_bytes = static_cast<size_t>(phys_w) * 4u;
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        uint8_t*       dst = fb.data.data();

        if (mapped.RowPitch == static_cast<UINT>(phys_w * 4)) {
            std::memcpy(dst, src, fb.data.size());
        } else {
            for (int row = 0; row < phys_h; ++row) {
                std::memcpy(dst + row * row_bytes,
                            src + row * mapped.RowPitch,
                            row_bytes);
            }
        }

        impl_->context->Unmap(impl_->staging_tex, 0);
        impl_->duplication->ReleaseFrame();

        return fb;
    }
}

/**
 * @brief 解像度変更を通知して sws_ctx を再構成する
 *
 * DXGI Duplication 自体は継続使用し、スケーラーのみ更新する。
 *
 * @param new_width   新しい論理幅
 * @param new_height  新しい論理高さ
 * @return 常に true
 */
bool DxgiCaptureBackend::reconfigure(int new_width, int new_height) {
    logical_width_  = new_width;
    logical_height_ = new_height;

    if (impl_->sws_ctx &&
        impl_->physical_width == new_width &&
        impl_->physical_height == new_height)
    {
        // スケーリングが不要になった場合は sws_ctx を解放する
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    } else if (impl_->physical_width != 0) {
        // スケーリングが必要な場合は sws_ctx を再作成する
        if (impl_->sws_ctx) {
            sws_freeContext(impl_->sws_ctx);
            impl_->sws_ctx = nullptr;
        }

        if (impl_->physical_width != new_width || impl_->physical_height != new_height) {
            impl_->sws_ctx = sws_getContext(
                impl_->physical_width,  impl_->physical_height, AV_PIX_FMT_BGRA,
                new_width,              new_height,              AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
    }

    return true;
}

/**
 * @brief すべての COM リソースと sws_ctx を解放する
 *
 * 二重呼び出しに対して安全。
 */
void DxgiCaptureBackend::release() {
    if (!impl_) {
        return;
    }

    // sws_ctx を解放する
    if (impl_->sws_ctx) {
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    }

    // ステージングテクスチャを解放する
    if (impl_->staging_tex) {
        impl_->staging_tex->Release();
        impl_->staging_tex = nullptr;
    }

    // GPU ゼロコピーパス用テクスチャプールを解放する
    for (int i = 0; i < Impl::kZeroCopyPoolSize; ++i) {
        if (impl_->zero_copy_pool[i]) {
            impl_->zero_copy_pool[i]->Release();
            impl_->zero_copy_pool[i] = nullptr;
        }
    }
    impl_->zero_copy_pool_idx = 0;
    zero_copy_mode_.store(false);

    // デスクトップ複製インターフェースを解放する
    if (impl_->duplication) {
        impl_->duplication->Release();
        impl_->duplication = nullptr;
    }

    // D3D11 デバイスコンテキストを解放する
    if (impl_->context) {
        impl_->context->Release();
        impl_->context = nullptr;
    }

    // D3D11 デバイスを解放する
    if (impl_->device) {
        impl_->device->Release();
        impl_->device = nullptr;
    }
}

/**
 * @brief キャプチャに使用している D3D11 デバイスを返す
 * @return ID3D11Device* (型消去)。未初期化の場合は nullptr
 */
void* DxgiCaptureBackend::gpu_device() const {
    return impl_->device;
}

/**
 * @brief GPU ゼロコピーパスが利用可能かどうかを返す
 *
 * sws_ctx が構築されている（物理解像度と論理解像度が異なる）場合は
 * GPU 側でスケーリングを行う実装を持たないため false を返す。
 *
 * @return ゼロコピーパスが利用可能なら true
 */
bool DxgiCaptureBackend::supports_zero_copy() const {
    return impl_->device != nullptr && impl_->sws_ctx == nullptr;
}

/**
 * @brief ゼロコピーモードの有効・無効を切り替える
 * @param enabled true で有効化、false で無効化
 */
void DxgiCaptureBackend::set_zero_copy_mode(bool enabled) {
    zero_copy_mode_.store(enabled);
}

// ---------------------------------------------------------------------------
// プライベートメソッド
// ---------------------------------------------------------------------------

/**
 * @brief HMONITOR に対応する DXGI アダプターとアウトプットを検索して初期化する
 *
 * IDXGIFactory1 でアダプターを列挙し、各アダプターのアウトプットの
 * Monitor ハンドルが hmonitor と一致するものを見つけて D3D11 デバイスと
 * IDXGIOutputDuplication を作成する。
 *
 * @param hmonitor  対象 HMONITOR
 * @return 成功した場合 true
 */
bool DxgiCaptureBackend::find_and_init(HMONITOR hmonitor) {
    // DXGI ファクトリーを作成する
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        last_error_ = "CreateDXGIFactory1 failed";
        return false;
    }

    bool found = false;

    // アダプターを列挙する
    for (UINT adapter_idx = 0; ; ++adapter_idx) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(adapter_idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        // アウトプットを列挙する
        for (UINT output_idx = 0; ; ++output_idx) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(output_idx, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            // アウトプットのモニターハンドルを確認する
            DXGI_OUTPUT_DESC desc{};
            hr = output->GetDesc(&desc);

            if (SUCCEEDED(hr) && desc.Monitor == hmonitor) {
                // 一致するアウトプットを発見。D3D11 デバイスを作成する
                D3D_FEATURE_LEVEL feature_level{};
                hr = D3D11CreateDevice(
                    adapter,
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    0,
                    nullptr, 0,
                    D3D11_SDK_VERSION,
                    &impl_->device,
                    &feature_level,
                    &impl_->context
                );

                if (FAILED(hr) || !impl_->device || !impl_->context) {
                    last_error_ = "D3D11CreateDevice failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    output->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // IDXGIOutput から IDXGIOutput1 を取得する
                IDXGIOutput1* output1 = nullptr;
                hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                output->Release();
                output = nullptr;

                if (FAILED(hr) || !output1) {
                    last_error_ = "QueryInterface for IDXGIOutput1 failed";
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // IDXGIOutputDuplication を取得する
                hr = output1->DuplicateOutput(impl_->device, &impl_->duplication);
                output1->Release();
                output1 = nullptr;

                if (FAILED(hr) || !impl_->duplication) {
                    last_error_ = "DuplicateOutput failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // 物理解像度を取得する
                DXGI_OUTDUPL_DESC dup_desc{};
                impl_->duplication->GetDesc(&dup_desc);
                impl_->physical_width  = static_cast<int>(dup_desc.ModeDesc.Width);
                impl_->physical_height = static_cast<int>(dup_desc.ModeDesc.Height);

                // ステージングテクスチャを作成する (CPU 読み取り用)
                D3D11_TEXTURE2D_DESC tex_desc{};
                tex_desc.Width              = dup_desc.ModeDesc.Width;
                tex_desc.Height             = dup_desc.ModeDesc.Height;
                tex_desc.MipLevels          = 1;
                tex_desc.ArraySize          = 1;
                tex_desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
                tex_desc.SampleDesc.Count   = 1;
                tex_desc.SampleDesc.Quality = 0;
                tex_desc.Usage              = D3D11_USAGE_STAGING;
                tex_desc.BindFlags          = 0;
                tex_desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
                tex_desc.MiscFlags          = 0;

                hr = impl_->device->CreateTexture2D(&tex_desc, nullptr, &impl_->staging_tex);
                if (FAILED(hr) || !impl_->staging_tex) {
                    last_error_ = "CreateTexture2D (staging) failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // GPU ゼロコピーパス用テクスチャプールを作成する
                // (D3D11_USAGE_DEFAULT・GPU 内のみで完結し CPU からはアクセスしない。
                //  BindFlags はエンコーダー側のハードウェアフレームコンテキストへの
                //  コピー元として利用できるよう SHADER_RESOURCE を指定する)
                D3D11_TEXTURE2D_DESC zc_desc = tex_desc;
                zc_desc.Usage          = D3D11_USAGE_DEFAULT;
                zc_desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
                zc_desc.CPUAccessFlags = 0;

                for (int i = 0; i < Impl::kZeroCopyPoolSize; ++i) {
                    hr = impl_->device->CreateTexture2D(&zc_desc, nullptr, &impl_->zero_copy_pool[i]);
                    if (FAILED(hr) || !impl_->zero_copy_pool[i]) {
                        last_error_ = "CreateTexture2D (zero-copy pool) failed: HRESULT=" +
                                      std::to_string(static_cast<long>(hr));
                        adapter->Release();
                        factory->Release();
                        return false;
                    }
                }
                impl_->zero_copy_pool_idx = 0;

                found = true;
                adapter->Release();
                factory->Release();
                return true;
            }

            if (output) {
                output->Release();
                output = nullptr;
            }
        }

        adapter->Release();
    }

    factory->Release();

    if (!found) {
        last_error_ = "No matching DXGI output found for the specified monitor";
    }

    return false;
}

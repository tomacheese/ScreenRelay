#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>  // ID3D10Multithread (NVENC が要求するマルチスレッド保護の有効化に使用)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include "encoder/encoder_controller.hpp"
#include <cstring>
#include <stdexcept>

/**
 * @brief EncoderController の内部実装
 *
 * FFmpeg のコーデックコンテキスト・スケーラー・フレーム・パケットを保持する。
 */
struct EncoderController::Impl {
    const AVCodec*  codec      = nullptr;  ///< 使用コーデック
    AVCodecContext* codec_ctx  = nullptr;  ///< コーデックコンテキスト
    SwsContext*     sws_ctx    = nullptr;  ///< ピクセルフォーマット変換コンテキスト
    AVFrame*        yuv_frame  = nullptr;  ///< YUV フレームバッファ
    AVPacket*       pkt        = nullptr;  ///< 再利用可能なパケット
    int64_t         frame_count          = 0;  ///< 送信フレームカウンター
    int64_t         first_frame_time_us_ = 0;  ///< 最初のフレームの壁時計時刻 (μs, UNIX エポック起点)
    int64_t         last_pts_            = -1; ///< 直前フレームの PTS (単調増加を保証するために使用)
    std::string     codec_name;            ///< 選択されたコーデック名

    // --- GPU ゼロコピーパス (D3D11VA ハードウェアフレーム) 関連 ---
    AVBufferRef* hw_device_ctx        = nullptr;  ///< D3D11VA ハードウェアデバイスコンテキスト
    AVBufferRef* hw_frames_ctx        = nullptr;  ///< D3D11VA ハードウェアフレームコンテキスト (BGRA テクスチャプール)
    AVFrame*     hw_frame             = nullptr;  ///< プールから取得する再利用可能なハードウェアフレーム
    bool         gpu_zero_copy_active = false;    ///< GPU ゼロコピーパスが確立されているか

    bool         force_next_idr       = false;    ///< 次フレームを IDR として強制出力するフラグ

    /**
     * @brief 次に送信するフレームの PTS を計算し、内部カウンターを更新する
     *
     * 最初のフレームの壁時計時刻を基点として実キャプチャ間隔を反映した PTS を
     * 算出する（frame_count++ による固定間隔仮定では RTCP SR と VLC の
     * タイムスタンプ整合性が崩れるため）。連続するフレームで PTS が同一または
     * 逆転しないよう単調増加を保証する。CPU パス・GPU パス共通で使用する。
     *
     * @param meta フレームメタデータ
     * @param fps  フレームレート
     * @return 算出された PTS
     */
    int64_t compute_next_pts(const FrameMeta& meta, int fps) {
        if (frame_count == 0) {
            first_frame_time_us_ = meta.timestamp_us;
        }
        int64_t elapsed_us = meta.timestamp_us - first_frame_time_us_;
        int64_t new_pts    = elapsed_us * static_cast<int64_t>(fps) / 1000000LL;

        // PTS は単調増加を維持する。RTP タイムスタンプの 32-bit ラップアラウンドは
        // FFmpeg RTP マルチプレクサが uint32_t への切り捨てで透過的に処理するため、
        // PTS 側でリセットする必要はない。仮にリセットすると AVPacket の PTS/DTS が
        // 巻き戻り av_interleaved_write_frame の non-monotonic timestamp エラーを
        // 引き起こす（RFC 3550 のラップアラウンドは RTP ヘッダ層の仕様であり、
        // FFmpeg の AVPacket タイムスタンプは int64_t で単調増加が前提）。
        if (new_pts <= last_pts_) new_pts = last_pts_ + 1;
        last_pts_ = new_pts;
        frame_count++;
        return new_pts;
    }
};

EncoderController::EncoderController()
    : impl_(std::make_unique<Impl>()) {}

EncoderController::~EncoderController() { reset(); }

/**
 * @brief コーデックがサポートする最適なピクセルフォーマットを選択する
 *
 * YUV420P を優先し、次に NV12、最後にコーデックの先頭フォーマットを返す。
 *
 * @param codec 対象コーデック
 * @return 選択されたピクセルフォーマット
 */
static AVPixelFormat pick_pix_fmt(const AVCodec* codec) {
    const AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
    // FFmpeg 7+ 向けの新しい API
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, (const void**)&fmts, nullptr) < 0)
        fmts = nullptr;
#else
    fmts = codec->pix_fmts;
#endif
    if (!fmts) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    return fmts[0];
}

/**
 * @brief GPU ゼロコピーパス (D3D11VA ハードウェアフレーム + NVENC) の確立を試みる
 *
 * shared_device 上に AVHWDeviceContext を構築し、BGRA テクスチャプールを持つ
 * AVHWFramesContext を割り当てたうえで、AV_PIX_FMT_D3D11 入力の NVENC を開く。
 * NVENC は D3D11 BGRA テクスチャを直接受け取り、内部 (GPU 上) で NV12 へ
 * 変換してエンコードできるため、CPU 側での色空間変換が完全に不要になる。
 *
 * 失敗した場合は確保したリソースをすべて解放し false を返す。
 * 呼び出し元は通常の CPU パス（sws_scale 経由）にフォールバックできる。
 *
 * @param codec          NVENC コーデック (h264_nvenc を想定)
 * @param shared_device  キャプチャバックエンドと共有する ID3D11Device*
 * @param config         エンコーダー設定
 * @param width          フレーム幅 (px)
 * @param height         フレーム高さ (px)
 * @param[out] out_ctx          開いたコーデックコンテキスト
 * @param[out] out_hw_device    AVHWDeviceContext への参照
 * @param[out] out_hw_frames    AVHWFramesContext への参照 (BGRA テクスチャプール)
 * @param[out] out_hw_frame     プールから取得した再利用可能なハードウェアフレーム
 * @param[out] error            エラーメッセージ出力先
 * @return 成功した場合 true
 */
static bool try_init_gpu_zero_copy(const AVCodec* codec, ID3D11Device* shared_device,
                                   const EncoderConfig& config,
                                   uint32_t width, uint32_t height,
                                   AVCodecContext*& out_ctx,
                                   AVBufferRef*& out_hw_device,
                                   AVBufferRef*& out_hw_frames,
                                   AVFrame*& out_hw_frame,
                                   std::string& error) {
    out_ctx = nullptr;
    out_hw_device = nullptr;
    out_hw_frames = nullptr;
    out_hw_frame  = nullptr;

    if (!shared_device) {
        error = "shared D3D11 device is null";
        return false;
    }

    // --- AVHWDeviceContext: キャプチャと同一の D3D11 デバイスをラップする ---
    // デバイスは共有元 (DxgiCaptureBackend) が解放を管理しているため、
    // FFmpeg 側のコンテキスト解放時に誤って Release されないよう AddRef する。
    AVBufferRef* hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx) {
        error = "av_hwdevice_ctx_alloc(D3D11VA) failed";
        return false;
    }
    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
    auto* d3d11_hwctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
    shared_device->AddRef();
    d3d11_hwctx->device = shared_device;

    // NVENC は D3D11 デバイスのマルチスレッド保護 (ID3D10Multithread) が
    // 有効化されていることを要求する。FFmpeg が独自にデバイスを作成する場合
    // (d3d11va_device_create) は SetMultithreadProtected(TRUE) を呼んでいるが、
    // 既存デバイスを共有する本パスではその初期化が行われないため、ここで
    // 明示的に有効化しておく。怠ると avcodec_send_frame が
    // "Failed locking bitstream buffer: invalid device" で失敗する。
    {
        ID3D10Multithread* multithread = nullptr;
        HRESULT mt_hr = shared_device->QueryInterface(__uuidof(ID3D10Multithread),
                                                       reinterpret_cast<void**>(&multithread));
        if (SUCCEEDED(mt_hr) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
    }

    int ret = av_hwdevice_ctx_init(hw_device_ctx);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = "av_hwdevice_ctx_init failed: " + std::string(buf);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    // --- AVHWFramesContext: BGRA テクスチャプールを構築する ---
    // sw_format=BGRA を指定することで、DXGI Desktop Duplication が出力する
    // BGRA テクスチャをそのまま (色空間変換なしで) NVENC に渡せる。
    AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ctx) {
        error = "av_hwframe_ctx_alloc failed";
        av_buffer_unref(&hw_device_ctx);
        return false;
    }
    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx->data);
    frames_ctx->format            = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format         = AV_PIX_FMT_BGRA;
    frames_ctx->width             = static_cast<int>(width);
    frames_ctx->height            = static_cast<int>(height);
    frames_ctx->initial_pool_size = 4;

    auto* d3d11_frames = static_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
    d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ret = av_hwframe_ctx_init(hw_frames_ctx);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = "av_hwframe_ctx_init failed: " + std::string(buf);
        av_buffer_unref(&hw_frames_ctx);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    // --- コーデックコンテキストを D3D11 ハードウェアフレーム入力で開く ---
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        error = "avcodec_alloc_context3 failed";
        av_buffer_unref(&hw_frames_ctx);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    ctx->codec_id      = codec->id;
    ctx->bit_rate      = static_cast<int64_t>(config.bitrate_kbps) * 1000;
    ctx->width         = static_cast<int>(width);
    ctx->height        = static_cast<int>(height);
    ctx->time_base     = {1, config.fps};
    ctx->framerate     = {config.fps, 1};
    ctx->gop_size      = config.gop_size;
    ctx->max_b_frames  = config.max_b_frames;
    ctx->pix_fmt       = AV_PIX_FMT_D3D11;
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    if (config.threads > 0) ctx->thread_count = config.threads;

    AVDictionary* opts = nullptr;
    if (!config.preset.empty())
        av_dict_set(&opts, "preset", config.preset.c_str(), 0);
    // GPU ゼロコピーパスは常に NVENC を使用する。NVENC は libx264 の
    // "tune=zerolatency" をサポートしない（CPU パスの is_hw_encoder 判定と同様に
    // tune は適用しない）。低遅延は rc=cbr + delay=0 で実現する。
    av_dict_set(&opts, "rc", "cbr", 0);
    av_dict_set(&opts, "delay", "0", 0);

    ret = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = "avcodec_open2 (GPU zero-copy) failed: " + std::string(buf);
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw_frames_ctx);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    // 再利用する AVFrame の入れ物を 1 つ確保しておく。
    // 実際のバッファ参照は encode 毎に av_frame_unref() + av_hwframe_get_buffer()
    // で取得し直す（プールからラウンドロビンで安全な書き込み先を得るため）。
    AVFrame* hw_frame = av_frame_alloc();
    if (!hw_frame) {
        error = "av_frame_alloc (hw_frame) failed";
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw_frames_ctx);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }
    ret = av_hwframe_get_buffer(hw_frames_ctx, hw_frame, 0);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = "av_hwframe_get_buffer failed: " + std::string(buf);
        av_frame_free(&hw_frame);
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw_frames_ctx);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    out_ctx       = ctx;
    out_hw_device = hw_device_ctx;
    out_hw_frames = hw_frames_ctx;
    out_hw_frame  = hw_frame;
    return true;
}

bool EncoderController::init(const EncoderConfig& config,
                              uint32_t width, uint32_t height,
                              std::string& error,
                              void* shared_d3d11_device) {
    reset();
    config_ = config;
    width_  = width;
    height_ = height;

    // 優先コーデックとフォールバックコーデックの試行リストを構築する
    std::vector<std::string> codecs_to_try = { config.codec };
    for (const auto& fb : config.fallback_codecs)
        codecs_to_try.push_back(fb);

    for (const auto& codec_name : codecs_to_try) {
        const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) continue;

        // NVENC かつ共有 D3D11 デバイスがある場合は、まず GPU ゼロコピーパスを試みる。
        // 成功すれば CPU 側の色空間変換 (sws_scale) とテクスチャ読み戻し (Map/memcpy)
        // を完全に回避できる。失敗した場合は通常の CPU パスにフォールバックする。
        if (shared_d3d11_device && codec_name.find("nvenc") != std::string::npos) {
            AVCodecContext* gpu_ctx       = nullptr;
            AVBufferRef*    gpu_hw_device = nullptr;
            AVBufferRef*    gpu_hw_frames = nullptr;
            AVFrame*        gpu_hw_frame  = nullptr;
            std::string     gpu_error;

            if (try_init_gpu_zero_copy(codec, static_cast<ID3D11Device*>(shared_d3d11_device),
                                       config, width, height,
                                       gpu_ctx, gpu_hw_device, gpu_hw_frames, gpu_hw_frame,
                                       gpu_error)) {
                impl_->codec      = codec;
                impl_->codec_ctx  = gpu_ctx;
                impl_->codec_name = codec_name;
                impl_->hw_device_ctx = gpu_hw_device;
                impl_->hw_frames_ctx = gpu_hw_frames;
                impl_->hw_frame      = gpu_hw_frame;
                impl_->gpu_zero_copy_active = true;

                impl_->pkt = av_packet_alloc();
                if (!impl_->pkt) {
                    error = "av_packet_alloc failed";
                    reset();
                    return false;
                }

                impl_->frame_count          = 0;
                impl_->first_frame_time_us_ = 0;
                impl_->last_pts_            = -1;
                return true;
            }

            // GPU ゼロコピーパス確立失敗。ログ用にエラーを保持しつつ
            // 通常の CPU パス (sws_scale 経由) で同じコーデック名を試す。
            error = codec_name + ": GPU zero-copy init failed (" + gpu_error
                    + "), falling back to CPU path";
        }

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        ctx->codec_id     = codec->id;
        ctx->bit_rate     = static_cast<int64_t>(config.bitrate_kbps) * 1000;
        ctx->width        = static_cast<int>(width);
        ctx->height       = static_cast<int>(height);
        ctx->time_base    = {1, config.fps};
        ctx->framerate    = {config.fps, 1};
        ctx->gop_size     = config.gop_size;
        ctx->max_b_frames = config.max_b_frames;
        ctx->pix_fmt      = pick_pix_fmt(codec);
        if (config.threads > 0) ctx->thread_count = config.threads;

        AVDictionary* opts = nullptr;

        // nvenc/amf/qsv/h264_mf はハードウェアエンコーダーとして扱う
        bool is_hw_encoder = (codec_name.find("nvenc") != std::string::npos ||
                              codec_name.find("amf")   != std::string::npos ||
                              codec_name.find("qsv")   != std::string::npos ||
                              codec_name == "h264_mf");

        if (!config.preset.empty())
            av_dict_set(&opts, "preset", config.preset.c_str(), 0);

        if (!is_hw_encoder) {
            // libx264/libx265 は tune と zerolatency をサポートする
            if (!config.tune.empty())
                av_dict_set(&opts, "tune", config.tune.c_str(), 0);
        } else if (codec_name.find("nvenc") != std::string::npos) {
            // NVENC 低遅延設定
            av_dict_set(&opts, "rc", "cbr", 0);
            av_dict_set(&opts, "delay", "0", 0);
        }

        int ret = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            avcodec_free_context(&ctx);
            char buf[256];
            av_strerror(ret, buf, sizeof(buf));
            error = codec_name + ": avcodec_open2 failed: " + buf;
            continue; // 次のフォールバックを試みる
        }

        impl_->codec     = codec;
        impl_->codec_ctx = ctx;
        impl_->codec_name = codec_name;

        // DXGI キャプチャは BGRA フォーマットで出力するため AV_PIX_FMT_BGRA を使用する
        impl_->sws_ctx = sws_getContext(
            static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
            static_cast<int>(width), static_cast<int>(height), ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!impl_->sws_ctx) {
            avcodec_free_context(&impl_->codec_ctx);
            error = "sws_getContext failed";
            continue;
        }

        impl_->yuv_frame = av_frame_alloc();
        if (!impl_->yuv_frame) {
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_frame_alloc failed";
            return false;
        }
        impl_->yuv_frame->format = ctx->pix_fmt;
        impl_->yuv_frame->width  = static_cast<int>(width);
        impl_->yuv_frame->height = static_cast<int>(height);
        ret = av_frame_get_buffer(impl_->yuv_frame, 32);
        if (ret < 0) {
            av_frame_free(&impl_->yuv_frame);
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_frame_get_buffer failed";
            return false;
        }

        impl_->pkt = av_packet_alloc();
        if (!impl_->pkt) {
            av_frame_free(&impl_->yuv_frame);
            sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr;
            avcodec_free_context(&impl_->codec_ctx);
            error = "av_packet_alloc failed";
            return false;
        }

        impl_->frame_count          = 0;
        impl_->first_frame_time_us_ = 0;
        impl_->last_pts_            = -1;
        return true;
    }

    if (error.empty()) {
        error = "No encoder found for '" + config.codec + "' or any fallback";
    }
    return false;
}

/**
 * @brief エンコーダーからパケットを取り出してリストに追加する
 *
 * EAGAIN または EOF に達するまで avcodec_receive_packet を繰り返す。
 *
 * @param ctx コーデックコンテキスト
 * @param pkt 再利用可能なパケット
 * @param out 出力パケットリスト
 * @param fps タイムベース計算に使用する fps
 * @return 成功した場合 true
 */
static bool drain_encoder(AVCodecContext* ctx, AVPacket* pkt,
                           std::vector<EncodedPacket>& out, int fps) {
    while (true) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts          = pkt->pts;
        ep.dts          = pkt->dts;
        ep.duration     = pkt->duration > 0 ? pkt->duration : 1;
        ep.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        ep.time_base_num = 1;
        ep.time_base_den = fps;
        out.push_back(std::move(ep));
        av_packet_unref(pkt);
    }
    return true;
}

bool EncoderController::encode(const FrameBuffer& frame,
                                const FrameMeta& meta,
                                std::vector<EncodedPacket>& out_packets,
                                bool content_changed) {
    if (!impl_->codec_ctx) return false;

    // --- GPU ゼロコピーパス ---
    // frame に GPU テクスチャが設定されている場合、CPU 側の色空間変換を
    // 介さずそのままハードウェアエンコーダーに渡す。
    //
    // 接続直後は、DxgiCaptureBackend がゼロコピーモードへ切り替わる直前に
    // キャプチャされた CPU フレーム（gpu_texture を持たず BGRA データのみを
    // 保持するもの）が freeze_buf として渡されてくることがある。
    // DXGI Desktop Duplication は画面に変化が無い限り新規フレームを返さない
    // ため、アイドル状態のモニターではこの過渡的な状態が数十秒以上続く
    // ことがあり、単純にスキップするとその間ストリームへ何も送出されなくなる
    // （IDR はおろか映像そのものが届かない）。
    // そのため gpu_texture の有無に関わらず encode_gpu_zero_copy() に委譲し、
    // CPU フレームの場合は内部で UpdateSubresource により直接アップロードする。
    if (impl_->gpu_zero_copy_active) {
        return encode_gpu_zero_copy(frame, meta, out_packets);
    }

    // --- CPU パス (sws_scale 経由) ---
    if (!impl_->sws_ctx || !impl_->yuv_frame) return false;

    // ピクセル内容が変化している場合のみ BGRA → コーデックのピクセルフォーマット
    // 変換 (sws_scale) を行う。
    //
    // 静止画面のフリーズフレーム再送 (do_streaming_loop 参照) では同一の
    // BGRA データを設定 fps で繰り返しエンコードするが、変換結果も毎回同一になる。
    // sws_scale は解像度に比例した CPU コストがかかり（4K で約 12ms/フレーム、
    // 60fps 時に 1 コアの 7 割以上を占有する実測値あり）、これを毎フレーム
    // 繰り返すのは純粋な無駄である。直前の変換済み YUV フレームをそのまま
    // 再利用することで、エンコーダーへは新しい PTS でフレームを送り続けつつ
    // 変換コストを排除する。
    if (content_changed || impl_->frame_count == 0) {
        const uint8_t* in_data[1]  = { frame.data.data() };
        int            in_stride[1] = { static_cast<int>(frame.width) * 4 };

        int make_writable_ret = av_frame_make_writable(impl_->yuv_frame);
        if (make_writable_ret < 0) return false;

        sws_scale(impl_->sws_ctx,
                  in_data, in_stride, 0, static_cast<int>(frame.height),
                  impl_->yuv_frame->data, impl_->yuv_frame->linesize);
    }

    impl_->yuv_frame->pts = impl_->compute_next_pts(meta, config_.fps);

    // IDR 強制フラグが立っている場合はキーフレームを要求し、フラグを消費する。
    // RTSP 再接続後に即座に完全フレームが届くようにするため。
    impl_->yuv_frame->pict_type = impl_->force_next_idr
        ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    impl_->force_next_idr = false;

    int ret = avcodec_send_frame(impl_->codec_ctx, impl_->yuv_frame);
    if (ret < 0) return false;

    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

/**
 * @brief GPU ゼロコピーパスでフレームをエンコードする
 *
 * 実装ノート:
 * - frame.gpu_texture (DxgiCaptureBackend のテクスチャプール上の BGRA テクスチャ)
 *   が設定されている通常ケースでは、ハードウェアフレームコンテキストの
 *   プールテクスチャへ CopySubresourceRegion で GPU 内コピーする。これは
 *   GPU メモリ帯域のみに依存する処理で、CPU は関与せずサブミリ秒で完了する
 *   （sws_scale の 4K 約 12ms と比べて無視できるコスト）。
 * - frame.gpu_texture が無く frame.data (CPU 側 BGRA データ) のみを持つ
 *   過渡的なフレーム（接続直後、DxgiCaptureBackend がゼロコピーモードへ
 *   切り替わる直前にキャプチャされたもの）の場合は UpdateSubresource で
 *   直接アップロードする。DXGI Desktop Duplication は画面に変化が無い限り
 *   新規フレームを返さないため、これを行わずスキップしてしまうと、
 *   アイドル状態のモニターでは数十秒〜それ以上ストリームに何も
 *   送出されない事態になる。
 * - NVENC は送信したテクスチャ (D3D11 リソース) を非同期に処理するため、
 *   単一の AVFrame を使い回してコピー先に書き込み続けると、エンコーダーが
 *   まだ参照中のリソースを上書き登録することになり
 *   "EncodePicture failed!: invalid param" で失敗する。
 *   そのため毎フレーム impl_->hw_frame を unref したうえで
 *   av_hwframe_get_buffer() でプールから新しい参照を取得し、
 *   参照カウントに基づくラウンドロビンで安全な書き込み先を得る。
 */
bool EncoderController::encode_gpu_zero_copy(const FrameBuffer& frame,
                                              const FrameMeta& meta,
                                              std::vector<EncodedPacket>& out_packets) {
    if (!impl_->hw_frames_ctx || !impl_->hw_frame) return false;

    // プールから新しい参照を取得する（前のフレームの参照は解放され、
    // エンコーダーが使用中であれば参照カウントにより保持され続ける）。
    av_frame_unref(impl_->hw_frame);
    int ret = av_hwframe_get_buffer(impl_->hw_frames_ctx, impl_->hw_frame, 0);
    if (ret < 0) return false;

    auto* frames_ctx  = reinterpret_cast<AVHWFramesContext*>(impl_->hw_frames_ctx->data);
    auto* device_ctx  = reinterpret_cast<AVHWDeviceContext*>(frames_ctx->device_ref->data);
    auto* d3d11_hwctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

    ID3D11Texture2D* dst_tex = reinterpret_cast<ID3D11Texture2D*>(impl_->hw_frame->data[0]);
    UINT             dst_idx = static_cast<UINT>(reinterpret_cast<intptr_t>(impl_->hw_frame->data[1]));
    if (!dst_tex) return false;

    // lock/unlock は AVD3D11VADeviceContext が内部状態へのアクセスを
    // 保護するために提供しているコールバックであり、推奨される作法に従う。
    if (d3d11_hwctx->lock) d3d11_hwctx->lock(d3d11_hwctx->lock_ctx);
    if (frame.gpu_texture) {
        // 通常パス: GPU 内でのテクスチャ間コピー (CPU 同期を伴わない)
        ID3D11Texture2D* src_tex = static_cast<ID3D11Texture2D*>(frame.gpu_texture.get());
        d3d11_hwctx->device_context->CopySubresourceRegion(
            dst_tex, dst_idx, 0, 0, 0, src_tex, 0, nullptr);
    } else if (!frame.data.empty()) {
        // 接続直後の過渡的な CPU フレーム（DxgiCaptureBackend がゼロコピー
        // モードへ切り替わる直前にキャプチャしたもの）。
        // DXGI Desktop Duplication は画面に変化が無い限り新規フレームを
        // 返さないため、アイドル状態のモニターではこの状況が数十秒〜それ以上
        // 継続することがある。その間ストリームへ一切何も送出されなくなる
        // （IDR はおろか映像そのものが届かない）事態を避けるため、
        // CPU 側で保持している直近の BGRA ピクセルデータを UpdateSubresource で
        // 直接 GPU テクスチャへアップロードしてエンコードする
        // （sw_format=BGRA のため sws_scale による色空間変換は不要）。
        UINT row_pitch = static_cast<UINT>(frame.width) * 4;
        if (row_pitch == 0) {
            // width=0 の不正フレームは UpdateSubresource に渡さず早期リターン
            if (d3d11_hwctx->unlock) d3d11_hwctx->unlock(d3d11_hwctx->lock_ctx);
            return false;
        }
        d3d11_hwctx->device_context->UpdateSubresource(
            dst_tex, dst_idx, nullptr, frame.data.data(), row_pitch, 0);
    } else {
        if (d3d11_hwctx->unlock) d3d11_hwctx->unlock(d3d11_hwctx->lock_ctx);
        return false;
    }
    if (d3d11_hwctx->unlock) d3d11_hwctx->unlock(d3d11_hwctx->lock_ctx);

    impl_->hw_frame->pts = impl_->compute_next_pts(meta, config_.fps);

    // IDR 強制フラグが立っている場合はキーフレームを要求し、フラグを消費する。
    // av_hwframe_get_buffer() 後の hw_frame は既に pict_type = NONE にリセット
    // されているが、明示的に設定することで意図を明確にする。
    impl_->hw_frame->pict_type = impl_->force_next_idr
        ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    impl_->force_next_idr = false;

    ret = avcodec_send_frame(impl_->codec_ctx, impl_->hw_frame);
    if (ret < 0) return false;

    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

bool EncoderController::flush(std::vector<EncodedPacket>& out_packets) {
    if (!impl_->codec_ctx) return true;
    int ret = avcodec_send_frame(impl_->codec_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return false;
    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

void EncoderController::reset() {
    if (impl_->pkt)       { av_packet_free(&impl_->pkt); }
    if (impl_->yuv_frame) { av_frame_free(&impl_->yuv_frame); }
    if (impl_->sws_ctx)   { sws_freeContext(impl_->sws_ctx); impl_->sws_ctx = nullptr; }
    if (impl_->hw_frame)      { av_frame_free(&impl_->hw_frame); }
    if (impl_->hw_frames_ctx) { av_buffer_unref(&impl_->hw_frames_ctx); }
    if (impl_->hw_device_ctx) { av_buffer_unref(&impl_->hw_device_ctx); }
    if (impl_->codec_ctx) { avcodec_free_context(&impl_->codec_ctx); }
    impl_->frame_count          = 0;
    impl_->first_frame_time_us_ = 0;
    impl_->last_pts_            = -1;
    impl_->codec                = nullptr;
    impl_->codec_name.clear();
    impl_->gpu_zero_copy_active = false;
    impl_->force_next_idr       = false;
    width_  = 0;
    height_ = 0;
}

void EncoderController::request_next_idr() {
    impl_->force_next_idr = true;
}

int EncoderController::fps() const {
    return config_.fps;
}

int EncoderController::bitrate_kbps() const {
    return config_.bitrate_kbps;
}

int64_t EncoderController::first_frame_time_us() const {
    return impl_->first_frame_time_us_;
}

const std::string& EncoderController::selected_codec_name() const {
    return impl_->codec_name;
}

bool EncoderController::is_gpu_zero_copy_active() const {
    return impl_->gpu_zero_copy_active;
}

EncoderController::CodecInfo EncoderController::get_codec_info() const {
    CodecInfo ci;
    if (!impl_->codec_ctx) return ci;
    AVCodecContext* ctx = impl_->codec_ctx;
    ci.codec_id      = static_cast<int>(ctx->codec_id);
    ci.width         = ctx->width;
    ci.height        = ctx->height;
    ci.fps           = config_.fps;
    ci.bit_rate      = static_cast<int>(ctx->bit_rate);
    ci.time_base_num = ctx->time_base.num;
    ci.time_base_den = ctx->time_base.den;
    if (ctx->extradata && ctx->extradata_size > 0) {
        ci.extradata.assign(ctx->extradata,
                            ctx->extradata + ctx->extradata_size);
    }
    return ci;
}

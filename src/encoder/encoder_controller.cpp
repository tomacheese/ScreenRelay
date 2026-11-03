#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>  // ID3D10Multithread (multithread protection required by NVENC)

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

/** FFmpeg codec context, scaler, frames and packet state. */
struct EncoderController::Impl {
    const AVCodec*  codec      = nullptr;  ///< Selected encoder (NVENC or software fallback)
    AVCodecContext* codec_ctx  = nullptr;  ///< Opened codec context
    SwsContext*     sws_ctx    = nullptr;  ///< BGRA -> codec pixel format (CPU path only)
    AVFrame*        yuv_frame  = nullptr;  ///< Converted frame (CPU path only)
    AVPacket*       pkt        = nullptr;  ///< Reusable packet
    int64_t         frame_count          = 0;  ///< Frames submitted so far (used by compute_next_pts)
    int64_t         first_frame_time_us_ = 0;  ///< Wall-clock time of the first frame (us, UNIX epoch)
    int64_t         last_pts_            = -1; ///< PTS of the previous frame (keeps PTS strictly increasing)
    std::string     codec_name;            ///< Name of the codec actually opened

    // --- GPU zero-copy path (D3D11VA hardware frames) ---
    AVBufferRef* hw_device_ctx        = nullptr;  ///< D3D11VA device context
    AVBufferRef* hw_frames_ctx        = nullptr;  ///< BGRA texture pool
    AVFrame*     hw_frame             = nullptr;  ///< Holder frame; its buffer is re-fetched from the pool every encode
    bool         gpu_zero_copy_active = false;    ///< Zero-copy path established

    bool         force_next_idr       = false;    ///< Force the next frame to be an IDR

    /**
     * @brief Computes the PTS for the next frame and updates the counters.
     *
     * Derives the PTS from the real capture interval relative to the first
     * frame's wall-clock time; a fixed-interval frame_count++ would break
     * timestamp consistency between RTCP SR and players such as VLC.
     * Guarantees strictly increasing PTS. Shared by the CPU and GPU paths.
     *
     * @return The PTS in 1/fps units
     */
    int64_t compute_next_pts(const FrameMeta& meta, int fps) {
        if (frame_count == 0) {
            first_frame_time_us_ = meta.timestamp_us;
        }
        int64_t elapsed_us = meta.timestamp_us - first_frame_time_us_;
        int64_t new_pts    = elapsed_us * static_cast<int64_t>(fps) / 1000000LL;

        // Keep PTS monotonic. The FFmpeg RTP muxer truncates to uint32_t, so the
        // 32-bit RTP timestamp wraparound is handled transparently; resetting PTS
        // here would rewind AVPacket PTS/DTS (int64_t, assumed monotonic) and
        // make av_interleaved_write_frame fail with a non-monotonic timestamp error.
        if (new_pts <= last_pts_) new_pts = last_pts_ + 1;
        last_pts_ = new_pts;
        frame_count++;
        return new_pts;
    }
};

EncoderController::EncoderController()
    : impl_(std::make_unique<Impl>()) {}

EncoderController::~EncoderController() { reset(); }

/** Picks a pixel format the codec supports: YUV420P, else NV12, else its first format. */
static AVPixelFormat pick_pix_fmt(const AVCodec* codec) {
    const AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
    // New API for FFmpeg 7+
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
 * @brief Tries to establish the GPU zero-copy path (D3D11VA hardware frames + NVENC).
 *
 * Builds an AVHWDeviceContext on shared_device and an AVHWFramesContext with a
 * BGRA texture pool, then opens NVENC with AV_PIX_FMT_D3D11 input. NVENC takes
 * the D3D11 BGRA texture directly and converts to NV12 on the GPU, so no CPU
 * color conversion is needed.
 *
 * On failure, frees everything it allocated and returns false; the caller can
 * fall back to the CPU path (sws_scale).
 *
 * @param codec          NVENC codec (expected: h264_nvenc)
 * @param shared_device  ID3D11Device* shared with the capture backend
 * @param[out] out_ctx          Opened codec context
 * @param[out] out_hw_device    AVHWDeviceContext reference
 * @param[out] out_hw_frames    AVHWFramesContext reference (BGRA texture pool)
 * @param[out] out_hw_frame     Reusable hardware frame holder
 * @param[out] error            Error message on failure
 * @return true on success
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

    // AVHWDeviceContext wrapping the capture backend's D3D11 device.
    // The backend owns the device, so AddRef it to balance the Release FFmpeg
    // performs when the context is freed.
    AVBufferRef* hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx) {
        error = "av_hwdevice_ctx_alloc(D3D11VA) failed";
        return false;
    }
    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
    auto* d3d11_hwctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
    shared_device->AddRef();
    d3d11_hwctx->device = shared_device;

    // NVENC requires multithread protection (ID3D10Multithread) on the D3D11
    // device. FFmpeg calls SetMultithreadProtected(TRUE) only when it creates the
    // device itself (d3d11va_device_create), not for a shared one, so enable it
    // here. Otherwise avcodec_send_frame fails with
    // "Failed locking bitstream buffer: invalid device".
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

    // AVHWFramesContext: BGRA texture pool. sw_format=BGRA lets the DXGI
    // Desktop Duplication BGRA texture go to NVENC without color conversion.
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

    // Open the codec context with D3D11 hardware-frame input.
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
    // NVENC does not support libx264's "tune=zerolatency", so no tune is applied
    // (same as the CPU path's is_hw_encoder branch). Low latency comes from
    // rc=cbr + delay=0.
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

    // One reusable AVFrame holder. Its buffer reference is re-fetched on every
    // encode (av_frame_unref + av_hwframe_get_buffer) to get a safe write target
    // from the pool.
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

    std::vector<std::string> codecs_to_try = { config.codec };
    for (const auto& fb : config.fallback_codecs)
        codecs_to_try.push_back(fb);

    for (const auto& codec_name : codecs_to_try) {
        const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) continue;

        // For NVENC with a shared D3D11 device, try the GPU zero-copy path first
        // (avoids sws_scale and the Map/memcpy readback); on failure, fall back
        // to the CPU path below.
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

            // Zero-copy setup failed. Keep the error for logging and retry the
            // same codec on the CPU path.
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

        bool is_hw_encoder = (codec_name.find("nvenc") != std::string::npos ||
                              codec_name.find("amf")   != std::string::npos ||
                              codec_name.find("qsv")   != std::string::npos ||
                              codec_name == "h264_mf");

        if (!config.preset.empty())
            av_dict_set(&opts, "preset", config.preset.c_str(), 0);

        if (!is_hw_encoder) {
            // Software encoders (libx264/libx265) support tune (e.g. zerolatency).
            if (!config.tune.empty())
                av_dict_set(&opts, "tune", config.tune.c_str(), 0);
        } else if (codec_name.find("nvenc") != std::string::npos) {
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
            continue; // try the next fallback
        }

        impl_->codec     = codec;
        impl_->codec_ctx = ctx;
        impl_->codec_name = codec_name;

        // DXGI capture outputs BGRA, hence AV_PIX_FMT_BGRA as the input format.
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
 * @brief Drains packets from the encoder into `out` until EAGAIN or EOF.
 *
 * @param fps Used as the packet time base (1/fps)
 * @return true on success, false on an encoder error
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

    // GPU zero-copy path.
    //
    // Right after connecting, the freeze_buf passed in may be a CPU frame
    // (BGRA data, no gpu_texture) captured just before DxgiCaptureBackend
    // switched to zero-copy mode. DXGI Desktop Duplication returns no new
    // frame while the screen is unchanged, so on an idle monitor this can last
    // tens of seconds, and skipping the frame would send nothing at all
    // (not even an IDR). So always delegate to encode_gpu_zero_copy(), which
    // uploads CPU frames with UpdateSubresource.
    if (impl_->gpu_zero_copy_active) {
        return encode_gpu_zero_copy(frame, meta, out_packets);
    }

    // CPU path (via sws_scale)
    if (!impl_->sws_ctx || !impl_->yuv_frame) return false;

    // Run the BGRA -> codec pixel format conversion (sws_scale) only when the
    // content changed. Freeze-frame resends on a static screen (see
    // do_streaming_loop) encode identical BGRA data at the configured fps, so
    // the conversion result is identical too. sws_scale costs CPU in proportion
    // to resolution (measured: ~12 ms/frame at 4K, over 70% of one core at
    // 60 fps), so reuse the previous YUV frame and just feed it with a new PTS.
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

    // Consume the force-IDR flag (requested after an RTSP reconnect so the
    // receiver gets a complete frame immediately).
    impl_->yuv_frame->pict_type = impl_->force_next_idr
        ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    impl_->force_next_idr = false;

    int ret = avcodec_send_frame(impl_->codec_ctx, impl_->yuv_frame);
    if (ret < 0) return false;

    return drain_encoder(impl_->codec_ctx, impl_->pkt, out_packets, config_.fps);
}

/**
 * Implementation notes:
 * - Normal case (frame.gpu_texture set, a BGRA texture from
 *   DxgiCaptureBackend's pool): GPU-to-GPU CopySubresourceRegion into a pool
 *   texture of the hardware frames context. No CPU involvement; negligible
 *   next to sws_scale.
 * - Transitional frame with only frame.data (CPU BGRA captured just before
 *   DxgiCaptureBackend switched to zero-copy mode): upload with
 *   UpdateSubresource. DXGI Desktop Duplication returns no new frame while
 *   the screen is unchanged, so skipping it would send nothing for tens of
 *   seconds or more on an idle monitor.
 * - NVENC processes submitted D3D11 resources asynchronously. Reusing one
 *   AVFrame as the copy destination would overwrite a resource the encoder
 *   still references and fail with "EncodePicture failed!: invalid param".
 *   So unref hw_frame every frame and get a fresh reference from the pool
 *   with av_hwframe_get_buffer(), which hands out buffers round-robin by
 *   reference count.
 */
bool EncoderController::encode_gpu_zero_copy(const FrameBuffer& frame,
                                              const FrameMeta& meta,
                                              std::vector<EncodedPacket>& out_packets) {
    if (!impl_->hw_frames_ctx || !impl_->hw_frame) return false;

    // Fetch a fresh pool reference (the previous one is dropped here; if the
    // encoder still uses it, the reference count keeps it alive).
    av_frame_unref(impl_->hw_frame);
    int ret = av_hwframe_get_buffer(impl_->hw_frames_ctx, impl_->hw_frame, 0);
    if (ret < 0) return false;

    auto* frames_ctx  = reinterpret_cast<AVHWFramesContext*>(impl_->hw_frames_ctx->data);
    auto* device_ctx  = reinterpret_cast<AVHWDeviceContext*>(frames_ctx->device_ref->data);
    auto* d3d11_hwctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

    ID3D11Texture2D* dst_tex = reinterpret_cast<ID3D11Texture2D*>(impl_->hw_frame->data[0]);
    UINT             dst_idx = static_cast<UINT>(reinterpret_cast<intptr_t>(impl_->hw_frame->data[1]));
    if (!dst_tex) return false;

    // Hold the AVD3D11VADeviceContext lock; it is the same lock
    // DxgiCaptureBackend takes around its CopyResource on the shared context.
    if (d3d11_hwctx->lock) d3d11_hwctx->lock(d3d11_hwctx->lock_ctx);
    if (frame.gpu_texture) {
        // Normal path: GPU-side texture copy (no CPU sync)
        ID3D11Texture2D* src_tex = static_cast<ID3D11Texture2D*>(frame.gpu_texture.get());
        d3d11_hwctx->device_context->CopySubresourceRegion(
            dst_tex, dst_idx, 0, 0, 0, src_tex, 0, nullptr);
    } else if (!frame.data.empty()) {
        // Transitional CPU frame (see the implementation notes above): upload
        // the BGRA pixels directly (sw_format=BGRA, so no sws_scale needed).
        UINT row_pitch = static_cast<UINT>(frame.width) * 4;
        if (row_pitch == 0) {
            // Reject an invalid zero-width frame instead of passing it to UpdateSubresource.
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

    // Consume the force-IDR flag; pict_type is always set explicitly.
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

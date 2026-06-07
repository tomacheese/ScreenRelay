#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
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

bool EncoderController::init(const EncoderConfig& config,
                              uint32_t width, uint32_t height,
                              std::string& error) {
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
    if (!impl_->codec_ctx || !impl_->sws_ctx || !impl_->yuv_frame) return false;

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

    // 最初のフレームの壁時計時刻を基点として PTS を計算する。
    // frame_count++ による固定間隔仮定ではなく実キャプチャ間隔を反映させることで
    // RTCP SR と VLC のタイムスタンプ整合性を保つ。
    if (impl_->frame_count == 0) {
        impl_->first_frame_time_us_ = meta.timestamp_us;
    }
    int64_t elapsed_us = meta.timestamp_us - impl_->first_frame_time_us_;
    int64_t new_pts    = elapsed_us * static_cast<int64_t>(config_.fps) / 1000000LL;
    // 連続するフレームで PTS が同一または逆転しないよう単調増加を保証する
    if (new_pts <= impl_->last_pts_) new_pts = impl_->last_pts_ + 1;
    impl_->yuv_frame->pts = new_pts;
    impl_->last_pts_      = new_pts;
    impl_->frame_count++;

    int ret = avcodec_send_frame(impl_->codec_ctx, impl_->yuv_frame);
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
    if (impl_->codec_ctx) { avcodec_free_context(&impl_->codec_ctx); }
    impl_->frame_count          = 0;
    impl_->first_frame_time_us_ = 0;
    impl_->last_pts_            = -1;
    impl_->codec                = nullptr;
    impl_->codec_name.clear();
    width_  = 0;
    height_ = 0;
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

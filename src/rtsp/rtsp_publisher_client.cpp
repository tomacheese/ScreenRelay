#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include "rtsp/rtsp_publisher_client.hpp"
#include <cstring>

/**
 * @brief RtspPublisherClient の内部実装
 *
 * FFmpeg の AVFormatContext・AVStream・AVPacket を保持する。
 */
struct RtspPublisherClient::Impl {
    AVFormatContext* fmt_ctx      = nullptr;  ///< フォーマットコンテキスト
    AVStream*        video_stream = nullptr;  ///< ビデオストリーム
    AVPacket*        pkt          = nullptr;  ///< 再利用可能なパケット
    int              fps          = 60;       ///< フレームレート (タイムスタンプ変換用)
};

RtspPublisherClient::RtspPublisherClient()
    : impl_(std::make_unique<Impl>()) {}

RtspPublisherClient::~RtspPublisherClient() { disconnect(); }

bool RtspPublisherClient::connect(const std::string& url,
                                   const RtspConfig& config,
                                   const EncoderController::CodecInfo& codec_info,
                                   std::string& error) {
    disconnect();

    avformat_network_init();

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_alloc_output_context2(
        &fmt_ctx, nullptr, "rtsp", url.c_str());

    if (ret < 0 || !fmt_ctx) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = std::string("avformat_alloc_output_context2 failed: ") + buf;
        return false;
    }

    // ビデオストリームを追加する
    AVStream* st = avformat_new_stream(fmt_ctx, nullptr);
    if (!st) {
        avformat_free_context(fmt_ctx);
        error = "avformat_new_stream failed";
        return false;
    }

    st->id                       = 0;
    st->codecpar->codec_type     = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id       = static_cast<AVCodecID>(codec_info.codec_id);
    st->codecpar->width          = codec_info.width;
    st->codecpar->height         = codec_info.height;
    st->codecpar->bit_rate       = codec_info.bit_rate;
    st->time_base                = {codec_info.time_base_num, codec_info.time_base_den};

    if (!codec_info.extradata.empty()) {
        st->codecpar->extradata = static_cast<uint8_t*>(
            av_mallocz(codec_info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!st->codecpar->extradata) {
            avformat_free_context(fmt_ctx);
            error = "Failed to allocate codec extradata buffer";
            return false;
        }
        std::memcpy(st->codecpar->extradata,
                    codec_info.extradata.data(),
                    codec_info.extradata.size());
        st->codecpar->extradata_size =
            static_cast<int>(codec_info.extradata.size());
    }

    // RTCP SR のウォールクロック基点を PTS=0 に対応する実時刻に設定する。
    // 再接続時は最初のフレームの時刻 (stream_start_us) を使うことで、
    // PTS が frame_count ベースでなく実キャプチャ時刻ベースになった後も
    // VLC 等の受信側に一貫した NTP-RTP 対応を提供する。
    fmt_ctx->start_time_realtime = (codec_info.stream_start_us != 0)
        ? codec_info.stream_start_us
        : av_gettime();

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);

    // タイムアウトはマイクロ秒単位で設定する
    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%d",
             config.connect_timeout_ms * 1000);
    av_dict_set(&opts, "timeout", timeout_str, 0);

    ret = avformat_write_header(fmt_ctx, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        avformat_free_context(fmt_ctx);
        error = std::string("RTSP connect (write_header) failed: ") + buf;
        return false;
    }

    impl_->fmt_ctx      = fmt_ctx;
    impl_->video_stream = st;
    impl_->fps          = codec_info.fps;
    connected_.store(true);

    // パケット送信ごとのヒープ割り当てを避けるため再利用可能な AVPacket を確保する
    impl_->pkt = av_packet_alloc();
    if (!impl_->pkt) {
        avio_closep(&impl_->fmt_ctx->pb);
        avformat_free_context(impl_->fmt_ctx);
        impl_->fmt_ctx = nullptr;
        impl_->video_stream = nullptr;
        connected_.store(false);
        error = "Failed to allocate reusable AVPacket";
        return false;
    }

    // 書き込みタイムアウトを設定して av_interleaved_write_frame が無限ブロックしないようにする
    if (impl_->fmt_ctx->pb)
        av_opt_set_int(impl_->fmt_ctx->pb, "rw_timeout",
                       static_cast<int64_t>(config.send_timeout_ms) * 1000,
                       AV_OPT_SEARCH_CHILDREN);

    return true;
}

bool RtspPublisherClient::send_packet(const EncodedPacket& pkt) {
    if (!connected_.load() || !impl_->fmt_ctx || !impl_->video_stream || !impl_->pkt) return false;

    AVPacket* av_pkt = impl_->pkt;
    av_packet_unref(av_pkt);  // 前回のデータバッファを解放する (未使用時は no-op)

    int ret = av_new_packet(av_pkt, static_cast<int>(pkt.data.size()));
    if (ret < 0) { return false; }

    std::memcpy(av_pkt->data, pkt.data.data(), pkt.data.size());
    av_pkt->pts          = pkt.pts;
    av_pkt->dts          = pkt.dts;
    av_pkt->duration     = pkt.duration;
    av_pkt->stream_index = impl_->video_stream->index;

    if (pkt.is_key_frame) av_pkt->flags |= AV_PKT_FLAG_KEY;

    // エンコーダーのタイムベースからストリームのタイムベースに変換する
    AVRational enc_tb = {pkt.time_base_num, pkt.time_base_den};
    av_packet_rescale_ts(av_pkt, enc_tb, impl_->video_stream->time_base);

    ret = av_interleaved_write_frame(impl_->fmt_ctx, av_pkt);
    // av_interleaved_write_frame は所有権を取得して unref するため、
    // パケット構造体はそのまま再利用できる

    if (ret < 0) {
        connected_.store(false);
        return false;
    }

    return true;
}

void RtspPublisherClient::disconnect() {
    if (!impl_->fmt_ctx) return;

    if (connected_.load()) {
        av_write_trailer(impl_->fmt_ctx);
    }
    // フォーマットコンテキストを解放する前に AVIOContext を明示的にクローズして
    // ソケット/ハンドルのリークと再接続時の干渉を防ぐ
    if (impl_->fmt_ctx->pb && impl_->fmt_ctx->oformat &&
        !(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&impl_->fmt_ctx->pb);
    }
    avformat_free_context(impl_->fmt_ctx);
    impl_->fmt_ctx      = nullptr;
    impl_->video_stream = nullptr;

    av_packet_free(&impl_->pkt);
    impl_->pkt = nullptr;

    connected_.store(false);
}

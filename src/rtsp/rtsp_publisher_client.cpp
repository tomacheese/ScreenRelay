#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include "rtsp/rtsp_publisher_client.hpp"
#include <cstring>

/** @brief FFmpeg format context, streams and reusable packets. */
struct RtspPublisherClient::Impl {
    AVFormatContext* fmt_ctx      = nullptr;  ///< Format context
    AVStream*        video_stream = nullptr;  ///< Video stream
    AVStream*        audio_stream = nullptr;  ///< Audio stream (nullptr if unused)
    AVPacket*        pkt          = nullptr;  ///< Reusable video packet
    AVPacket*        audio_pkt    = nullptr;  ///< Reusable audio packet
    int              fps          = 60;       ///< Frame rate from codec_info
};

RtspPublisherClient::RtspPublisherClient()
    : impl_(std::make_unique<Impl>()) {}

RtspPublisherClient::~RtspPublisherClient() { disconnect(); }

bool RtspPublisherClient::connect(const std::string& url,
                                   const RtspConfig& config,
                                   const EncoderController::CodecInfo& codec_info,
                                   std::string& error,
                                   const AudioCodecInfo* audio_info) {
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

    AVStream* audio_st = nullptr;
    if (audio_info) {
        audio_st = avformat_new_stream(fmt_ctx, nullptr);
        if (!audio_st) {
            avformat_free_context(fmt_ctx);
            error = "avformat_new_stream (audio) failed";
            return false;
        }

        audio_st->id                        = 1;
        audio_st->codecpar->codec_type       = AVMEDIA_TYPE_AUDIO;
        audio_st->codecpar->codec_id         = static_cast<AVCodecID>(audio_info->codec_id);
        audio_st->codecpar->sample_rate      = audio_info->sample_rate;
        audio_st->codecpar->bit_rate         = audio_info->bit_rate;
        av_channel_layout_default(&audio_st->codecpar->ch_layout, audio_info->channels);
        audio_st->time_base                  = {audio_info->time_base_num, audio_info->time_base_den};

        if (!audio_info->extradata.empty()) {
            audio_st->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(audio_info->extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!audio_st->codecpar->extradata) {
                avformat_free_context(fmt_ctx);
                error = "Failed to allocate audio codec extradata buffer";
                return false;
            }
            std::memcpy(audio_st->codecpar->extradata,
                        audio_info->extradata.data(),
                        audio_info->extradata.size());
            audio_st->codecpar->extradata_size =
                static_cast<int>(audio_info->extradata.size());
        }
    }

    // Wall-clock origin for RTCP SR: the real time corresponding to PTS=0. On
    // reconnect, stream_start_us (the first frame's time) is used so receivers such
    // as VLC keep a consistent NTP-RTP mapping now that PTS follows capture time
    // rather than a frame count.
    fmt_ctx->start_time_realtime = (codec_info.stream_start_us != 0)
        ? codec_info.stream_start_us
        : av_gettime();

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);

    // The timeout option is in microseconds
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
    impl_->audio_stream = audio_st;
    impl_->fps          = codec_info.fps;
    connected_.store(true);

    // Reusable packets avoid a heap allocation per sent packet
    impl_->pkt = av_packet_alloc();
    impl_->audio_pkt = audio_st ? av_packet_alloc() : nullptr;
    if (!impl_->pkt || (audio_st && !impl_->audio_pkt)) {
        avio_closep(&impl_->fmt_ctx->pb);
        avformat_free_context(impl_->fmt_ctx);
        impl_->fmt_ctx = nullptr;
        impl_->video_stream = nullptr;
        impl_->audio_stream = nullptr;
        av_packet_free(&impl_->pkt);
        av_packet_free(&impl_->audio_pkt);
        connected_.store(false);
        error = "Failed to allocate reusable AVPacket";
        return false;
    }

    // Write timeout (applies after the header) so av_interleaved_write_frame cannot block forever
    if (impl_->fmt_ctx->pb)
        av_opt_set_int(impl_->fmt_ctx->pb, "rw_timeout",
                       static_cast<int64_t>(config.send_timeout_ms) * 1000,
                       AV_OPT_SEARCH_CHILDREN);

    return true;
}

bool RtspPublisherClient::send_packet(const EncodedPacket& pkt) {
    if (!connected_.load() || !impl_->fmt_ctx || !impl_->video_stream || !impl_->pkt) return false;

    AVPacket* av_pkt = impl_->pkt;
    av_packet_unref(av_pkt);  // release the previous data buffer (no-op if unused)

    int ret = av_new_packet(av_pkt, static_cast<int>(pkt.data.size()));
    if (ret < 0) { return false; }

    std::memcpy(av_pkt->data, pkt.data.data(), pkt.data.size());
    av_pkt->pts          = pkt.pts;
    av_pkt->dts          = pkt.dts;
    av_pkt->duration     = pkt.duration;
    av_pkt->stream_index = impl_->video_stream->index;

    if (pkt.is_key_frame) av_pkt->flags |= AV_PKT_FLAG_KEY;

    AVRational enc_tb = {pkt.time_base_num, pkt.time_base_den};
    av_packet_rescale_ts(av_pkt, enc_tb, impl_->video_stream->time_base);

    ret = av_interleaved_write_frame(impl_->fmt_ctx, av_pkt);
    // av_interleaved_write_frame takes ownership and unrefs the packet, so the
    // AVPacket struct can be reused as-is

    if (ret < 0) {
        connected_.store(false);
        return false;
    }

    return true;
}

bool RtspPublisherClient::send_audio_packet(const EncodedPacket& pkt) {
    if (!connected_.load() || !impl_->fmt_ctx || !impl_->audio_stream || !impl_->audio_pkt)
        return false;

    AVPacket* av_pkt = impl_->audio_pkt;
    av_packet_unref(av_pkt);  // release the previous data buffer (no-op if unused)

    int ret = av_new_packet(av_pkt, static_cast<int>(pkt.data.size()));
    if (ret < 0) { return false; }

    std::memcpy(av_pkt->data, pkt.data.data(), pkt.data.size());
    av_pkt->pts          = pkt.pts;
    av_pkt->dts          = pkt.dts;
    av_pkt->duration     = pkt.duration;
    av_pkt->stream_index = impl_->audio_stream->index;
    av_pkt->flags       |= AV_PKT_FLAG_KEY;  // every AAC frame is independently decodable

    AVRational enc_tb = {pkt.time_base_num, pkt.time_base_den};
    av_packet_rescale_ts(av_pkt, enc_tb, impl_->audio_stream->time_base);

    ret = av_interleaved_write_frame(impl_->fmt_ctx, av_pkt);

    if (ret < 0) {
        connected_.store(false);
        return false;
    }

    return true;
}

bool RtspPublisherClient::has_audio_stream() const {
    return impl_->audio_stream != nullptr;
}

void RtspPublisherClient::disconnect() {
    if (!impl_->fmt_ctx) return;

    if (connected_.load()) {
        av_write_trailer(impl_->fmt_ctx);
    }
    // Close the AVIOContext explicitly before freeing the format context to avoid
    // leaking the socket/handle and interfering with a later reconnect
    if (impl_->fmt_ctx->pb && impl_->fmt_ctx->oformat &&
        !(impl_->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&impl_->fmt_ctx->pb);
    }
    avformat_free_context(impl_->fmt_ctx);
    impl_->fmt_ctx      = nullptr;
    impl_->video_stream = nullptr;
    impl_->audio_stream = nullptr;

    av_packet_free(&impl_->pkt);
    impl_->pkt = nullptr;
    av_packet_free(&impl_->audio_pkt);
    impl_->audio_pkt = nullptr;

    connected_.store(false);
}

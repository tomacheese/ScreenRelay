extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "audio/audio_encoder.hpp"
#include "common/time_utils.hpp"
#include <algorithm>
#include <cstring>

/** @brief FFmpeg AAC codec context, resampler and sample FIFO. */
struct AudioEncoderController::Impl {
    const AVCodec*  codec     = nullptr;  ///< AAC encoder
    AVCodecContext* codec_ctx = nullptr;  ///< Codec context
    SwrContext*     swr_ctx   = nullptr;  ///< Resampler context
    AVAudioFifo*    fifo      = nullptr;  ///< Accumulates samples up to the encoder frame size
    AVPacket*       pkt       = nullptr;  ///< Reusable packet
    AVFrame*        frame     = nullptr;  ///< Reusable PCM frame (used by drain_fifo_and_encode)

    int in_sample_rate = 48000;  ///< Input sample rate
    int in_channels    = 2;      ///< Input channel count
    AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_FLT;  ///< Input sample format (interleaved)

    int64_t samples_encoded = 0;  ///< Samples submitted to the encoder (drives pts)
    int64_t last_meta_us    = 0;  ///< Capture time of the most recent input buffer
};

AudioEncoderController::AudioEncoderController() : impl_(std::make_unique<Impl>()) {}

AudioEncoderController::~AudioEncoderController() { reset(); }

// Prefers FLTP; otherwise falls back to the codec's first supported format.
static AVSampleFormat pick_sample_fmt(const AVCodec* codec) {
    const AVSampleFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, (const void**)&fmts, nullptr) < 0)
        fmts = nullptr;
#else
    fmts = codec->sample_fmts;
#endif
    if (!fmts) return AV_SAMPLE_FMT_FLTP;
    for (const AVSampleFormat* p = fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
        if (*p == AV_SAMPLE_FMT_FLTP) return AV_SAMPLE_FMT_FLTP;
    return fmts[0];
}

bool AudioEncoderController::init(const AudioConfig& config, int in_sample_rate,
                                   int in_channels, bool in_is_float, std::string& error) {
    reset();

    impl_->in_sample_rate = in_sample_rate;
    impl_->in_channels    = in_channels;
    // WASAPI shared-mode mix formats are normally IEEE float32; anything else
    // (some capture devices) is assumed to be int16 PCM.
    impl_->in_sample_fmt  = in_is_float ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;

    impl_->codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!impl_->codec) {
        error = "AAC encoder not found (avcodec_find_encoder)";
        return false;
    }

    impl_->codec_ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->codec_ctx) {
        error = "avcodec_alloc_context3 failed";
        return false;
    }

    AVCodecContext* ctx = impl_->codec_ctx;
    ctx->sample_rate = config.sample_rate;
    ctx->bit_rate    = static_cast<int64_t>(config.bitrate_kbps) * 1000;
    ctx->sample_fmt  = pick_sample_fmt(impl_->codec);
    av_channel_layout_default(&ctx->ch_layout, config.channels);
    ctx->time_base = {1, config.sample_rate};

    int ret = avcodec_open2(ctx, impl_->codec, nullptr);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = std::string("avcodec_open2 (AAC) failed: ") + buf;
        reset();
        return false;
    }

    // Resampler: input PCM -> the encoder's required format.
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, impl_->in_channels);

    ret = swr_alloc_set_opts2(&impl_->swr_ctx,
                              &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
                              &in_layout, impl_->in_sample_fmt, impl_->in_sample_rate,
                              0, nullptr);
    av_channel_layout_uninit(&in_layout);
    if (ret < 0 || !impl_->swr_ctx) {
        error = "swr_alloc_set_opts2 failed";
        reset();
        return false;
    }
    ret = swr_init(impl_->swr_ctx);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        error = std::string("swr_init failed: ") + buf;
        reset();
        return false;
    }

    impl_->fifo = av_audio_fifo_alloc(ctx->sample_fmt, ctx->ch_layout.nb_channels, 1);
    if (!impl_->fifo) {
        error = "av_audio_fifo_alloc failed";
        reset();
        return false;
    }

    impl_->pkt = av_packet_alloc();
    if (!impl_->pkt) {
        error = "av_packet_alloc failed";
        reset();
        return false;
    }

    // Frame reused by drain_fifo_and_encode() to avoid per-call alloc/free;
    // frame_size is fixed once the encoder is open, so allocate the buffer once.
    const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 1024;
    impl_->frame = av_frame_alloc();
    if (!impl_->frame) {
        error = "av_frame_alloc failed";
        reset();
        return false;
    }
    impl_->frame->nb_samples  = frame_size;
    impl_->frame->format      = ctx->sample_fmt;
    impl_->frame->sample_rate = ctx->sample_rate;
    av_channel_layout_copy(&impl_->frame->ch_layout, &ctx->ch_layout);
    if (av_frame_get_buffer(impl_->frame, 0) < 0) {
        error = "av_frame_get_buffer failed";
        reset();
        return false;
    }

    impl_->samples_encoded = 0;
    return true;
}

// Encodes every full encoder frame currently held in the FIFO.
bool drain_fifo_and_encode(AudioEncoderController::Impl* impl,
                            std::vector<EncodedPacket>& out_packets) {
    AVCodecContext* ctx = impl->codec_ctx;
    const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 1024;

    while (av_audio_fifo_size(impl->fifo) >= frame_size) {
        AVFrame* frame = impl->frame;

        // The encoder may still hold a reference to the frame buffer after
        // avcodec_send_frame(); make it writable before overwriting (no copy if
        // there is no other reference).
        if (av_frame_make_writable(frame) < 0) return false;

        if (av_audio_fifo_read(impl->fifo, reinterpret_cast<void**>(frame->data),
                               frame_size) < frame_size) {
            return false;
        }

        frame->pts = impl->samples_encoded;
        impl->samples_encoded += frame_size;

        int ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) return false;

        while (true) {
            ret = avcodec_receive_packet(ctx, impl->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) return false;

            EncodedPacket ep;
            ep.data.assign(impl->pkt->data, impl->pkt->data + impl->pkt->size);
            ep.pts               = impl->pkt->pts;
            ep.dts               = impl->pkt->pts;
            ep.duration          = frame_size;
            ep.is_key_frame      = true;  // every AAC frame is independently decodable
            ep.time_base_num     = 1;
            ep.time_base_den     = ctx->sample_rate;
            ep.capture_timestamp_us = impl->last_meta_us;
            out_packets.push_back(std::move(ep));
            av_packet_unref(impl->pkt);
        }
    }
    return true;
}

bool AudioEncoderController::encode(const AudioBuffer& buf, const AudioMeta& meta,
                                     std::vector<EncodedPacket>& out_packets) {
    if (!impl_->codec_ctx || !impl_->swr_ctx || !impl_->fifo) return false;
    if (buf.frame_count == 0) return true;

    // A buf.data shorter than frame_count implies would cause an out-of-bounds
    // read in swr_convert; reject inconsistent capture buffers.
    const int in_bytes_per_sample =
        av_get_bytes_per_sample(impl_->in_sample_fmt);
    const size_t expected_bytes =
        static_cast<size_t>(buf.frame_count) *
        static_cast<size_t>(impl_->in_channels) *
        static_cast<size_t>(in_bytes_per_sample);
    if (in_bytes_per_sample <= 0 || buf.data.size() < expected_bytes) {
        return false;
    }

    impl_->last_meta_us = meta.timestamp_us;

    AVCodecContext* ctx = impl_->codec_ctx;

    // Interleaved PCM is passed to swr_convert as a single plane
    const uint8_t* in_data[1] = { buf.data.data() };

    // Upper bound on resampled output samples (includes the resampler's buffered delay)
    int64_t max_out_samples = av_rescale_rnd(
        swr_get_delay(impl_->swr_ctx, impl_->in_sample_rate) + buf.frame_count,
        ctx->sample_rate, impl_->in_sample_rate, AV_ROUND_UP);

    uint8_t** converted = nullptr;
    int linesize = 0;
    int ret = av_samples_alloc_array_and_samples(&converted, &linesize,
                                                 ctx->ch_layout.nb_channels,
                                                 static_cast<int>(max_out_samples),
                                                 ctx->sample_fmt, 0);
    if (ret < 0) return false;

    int converted_samples = swr_convert(impl_->swr_ctx, converted,
                                        static_cast<int>(max_out_samples),
                                        in_data, static_cast<int>(buf.frame_count));
    if (converted_samples < 0) {
        av_freep(&converted[0]);
        av_freep(&converted);
        return false;
    }

    if (converted_samples > 0) {
        if (av_audio_fifo_realloc(impl_->fifo,
                                  av_audio_fifo_size(impl_->fifo) + converted_samples) < 0) {
            av_freep(&converted[0]);
            av_freep(&converted);
            return false;
        }
        if (av_audio_fifo_write(impl_->fifo, reinterpret_cast<void**>(converted),
                                converted_samples) < converted_samples) {
            av_freep(&converted[0]);
            av_freep(&converted);
            return false;
        }
    }

    av_freep(&converted[0]);
    av_freep(&converted);

    return drain_fifo_and_encode(impl_.get(), out_packets);
}

bool AudioEncoderController::flush(std::vector<EncodedPacket>& out_packets) {
    if (!impl_->codec_ctx) return true;

    int ret = avcodec_send_frame(impl_->codec_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return false;

    while (true) {
        ret = avcodec_receive_packet(impl_->codec_ctx, impl_->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        EncodedPacket ep;
        ep.data.assign(impl_->pkt->data, impl_->pkt->data + impl_->pkt->size);
        ep.pts           = impl_->pkt->pts;
        ep.dts           = impl_->pkt->pts;
        ep.time_base_num = 1;
        ep.time_base_den = impl_->codec_ctx->sample_rate;
        out_packets.push_back(std::move(ep));
        av_packet_unref(impl_->pkt);
    }
    return true;
}

void AudioEncoderController::reset() {
    if (impl_->pkt)   { av_packet_free(&impl_->pkt); }
    if (impl_->frame) { av_frame_free(&impl_->frame); }
    if (impl_->fifo) { av_audio_fifo_free(impl_->fifo); impl_->fifo = nullptr; }
    if (impl_->swr_ctx) { swr_free(&impl_->swr_ctx); }
    if (impl_->codec_ctx) { avcodec_free_context(&impl_->codec_ctx); }
    impl_->codec            = nullptr;
    impl_->samples_encoded  = 0;
    impl_->last_meta_us     = 0;
}

AudioCodecInfo AudioEncoderController::get_codec_info() const {
    AudioCodecInfo info;
    if (!impl_->codec_ctx) return info;
    AVCodecContext* ctx = impl_->codec_ctx;
    info.codec_id      = static_cast<int>(ctx->codec_id);
    info.sample_rate   = ctx->sample_rate;
    info.channels      = ctx->ch_layout.nb_channels;
    info.bit_rate      = static_cast<int>(ctx->bit_rate);
    info.time_base_num = 1;
    info.time_base_den = ctx->sample_rate;
    if (ctx->extradata && ctx->extradata_size > 0) {
        info.extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    }
    return info;
}

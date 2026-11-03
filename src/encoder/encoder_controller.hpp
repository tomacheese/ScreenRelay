#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief FFmpeg H.264 encoder controller.
 *
 * Tries config.codec, then config.fallback_codecs in order.
 * Input frames are assumed to be BGRA.
 */
class EncoderController {
public:
    /** Does not open an encoder; see init(). */
    EncoderController();

    ~EncoderController();

    /**
     * @brief Initializes the encoder.
     *
     * Tries config.codec first, then config.fallback_codecs in order.
     *
     * If shared_d3d11_device is given and the codec is NVENC, first tries to
     * establish the GPU zero-copy path (BGRA textures passed straight to NVENC
     * through a D3D11VA hardware frames context), which avoids CPU color
     * conversion (sws_scale) and texture readback (Map/memcpy). On failure,
     * falls back to the CPU path.
     *
     * @param config              Encoder settings
     * @param width               Frame width (px)
     * @param height              Frame height (px)
     * @param error               Error message output
     * @param shared_d3d11_device Type-erased ID3D11Device* shared for the zero-copy
     *                            path; nullptr means CPU path only
     * @return true on success
     */
    bool init(const EncoderConfig& config, uint32_t width, uint32_t height,
              std::string& error, void* shared_d3d11_device = nullptr);

    /**
     * @brief Returns whether the GPU zero-copy path is active.
     *
     * True if init() succeeded through the GPU hardware frames context.
     */
    bool is_gpu_zero_copy_active() const;

    /**
     * @brief Encodes a BGRA frame.
     *
     * @param frame           BGRA frame buffer
     * @param meta            Frame metadata
     * @param out_packets     Receives the encoded packets
     * @param content_changed true if the pixels differ from the previously
     *                        encoded frame. With false, the sws_scale BGRA->YUV
     *                        conversion is skipped and the previous YUV frame
     *                        is reused, which greatly cuts CPU load when
     *                        resending freeze frames of a static screen.
     *                        Defaults to true (always convert). Ignored on the
     *                        GPU zero-copy path, where the GPU copy is cheap
     *                        enough to run every frame.
     *
     * If is_gpu_zero_copy_active() is true, the zero-copy path is always used,
     * whether or not frame.gpu_texture is set:
     * - gpu_texture set: GPU-side copy (CopySubresourceRegion)
     * - gpu_texture unset but frame.data present: CPU->GPU upload
     *   (UpdateSubresource). Covers the transitional CPU frame captured before
     *   the switch to zero-copy mode just after connecting, avoiding a video
     *   gap on idle monitors.
     * Otherwise frame.data is treated as a BGRA buffer and converted with
     * sws_scale before encoding (CPU path).
     *
     * @return true on success
     */
    bool encode(const FrameBuffer& frame, const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets,
                bool content_changed = true);

    /**
     * @brief Flushes the encoder, draining buffered frames at end of stream.
     * @param out_packets Receives the flushed packets
     * @return true on success
     */
    bool flush(std::vector<EncodedPacket>& out_packets);

    /** Frees all resources; call init() again to reuse. */
    void reset();

    uint32_t width()  const { return width_; }

    uint32_t height() const { return height_; }

    int fps() const;

    /** @brief Configured bitrate_kbps */
    int bitrate_kbps() const;

    /** @brief Codec info for initializing the RTSP stream (passed to RtspPublisherClient::connect()). */
    struct CodecInfo {
        int codec_id      = 0;   ///< AVCodecID value
        int width         = 0;   ///< Frame width (px)
        int height        = 0;   ///< Frame height (px)
        int fps           = 60;  ///< Frame rate
        int bit_rate      = 0;   ///< Bit rate (bps)
        int time_base_num = 1;   ///< Time base numerator
        int time_base_den = 60;  ///< Time base denominator
        std::vector<uint8_t> extradata;   ///< Extra data such as SPS/PPS
        /** @brief Wall-clock time (us, UNIX epoch) used as the RTCP SR NTP base.
         *  0 means it is set from av_gettime() at connect time.
         *  On reconnect, pass the first frame's time to keep RTCP SR consistent. */
        int64_t stream_start_us = 0;
    };

    /** @brief Returns the encoder's codec info (default-initialized before init()). */
    CodecInfo get_codec_info() const;

    /**
     * @brief Returns the codec actually opened.
     *
     * After a fallback this is the codec whose avcodec_open2() succeeded, not
     * config.codec. Empty before init().
     */
    const std::string& selected_codec_name() const;

    /**
     * @brief Returns the wall-clock time of the first frame.
     *
     * Used to set start_time_realtime correctly on RTSP reconnect.
     *
     * @return Time in us (UNIX epoch); 0 if no frame has been encoded yet
     */
    int64_t first_frame_time_us() const;

    /**
     * @brief Requests that the next frame be encoded as an IDR (key frame).
     *
     * Call after an RTSP reconnect so the receiver can show full video right
     * away. The IDR is inserted within the GOP without resetting the encoder,
     * so PTS continuity (and RTCP SR consistency) is preserved. The request is
     * consumed by the next encode() call.
     */
    void request_next_idr();

private:
    /**
     * @brief Encodes a frame on the GPU zero-copy path.
     *
     * Copies the D3D11 texture referenced by frame.gpu_texture into a pool
     * texture of the hardware frames context on the GPU (CopySubresourceRegion)
     * and passes it to avcodec_send_frame, with no CPU color conversion or
     * memory copy. A CPU-only frame (frame.data) is uploaded with UpdateSubresource.
     *
     * @param frame       Frame holding the GPU texture (or CPU data)
     * @param meta        Frame metadata
     * @param out_packets Receives the encoded packets
     * @return true on success
     */
    bool encode_gpu_zero_copy(const FrameBuffer& frame, const FrameMeta& meta,
                              std::vector<EncodedPacket>& out_packets);

    /** Pimpl */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint32_t width_  = 0;  ///< Frame width
    uint32_t height_ = 0;  ///< Frame height
    EncoderConfig config_; ///< Encoder settings
};

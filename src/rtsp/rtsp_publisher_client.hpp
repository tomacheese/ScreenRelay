#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include "encoder/encoder_controller.hpp"
#include <atomic>
#include <string>
#include <memory>

/**
 * @brief RTSP ANNOUNCE/RECORD client.
 *
 * Publishes encoded packets to an RTSP server over TCP using FFmpeg avformat.
 * One instance serves one URL, which is supplied by the caller.
 */
class RtspPublisherClient {
public:
    RtspPublisherClient();

    ~RtspPublisherClient();

    /**
     * @brief Connects to the RTSP server (ANNOUNCE/RECORD via avformat_write_header()).
     *
     * Disconnects any previous connection first. Packets can be sent afterwards.
     *
     * @param url        Full RTSP URL (rtsp://host:port/path)
     * @param config     RTSP settings (timeouts)
     * @param codec_info Video codec parameters
     * @param error      Receives an error message on failure
     * @param audio_info Audio codec parameters (nullptr adds no audio stream)
     * @return true on success
     */
    bool connect(const std::string& url,
                 const RtspConfig& config,
                 const EncoderController::CodecInfo& codec_info,
                 std::string& error,
                 const AudioCodecInfo* audio_info = nullptr);

    /**
     * @brief Sends an encoded video packet, rescaling its timestamps from the packet's
     *        time base to the stream's.
     * @return true on success; a write failure also makes is_connected() false
     */
    bool send_packet(const EncodedPacket& pkt);

    /**
     * @brief Sends an encoded audio packet.
     *
     * Returns false without doing anything if connect() was not given audio_info. As with
     * video, timestamps are rescaled from the packet's own time base.
     *
     * @return true on success; a write failure also makes is_connected() false
     */
    bool send_audio_packet(const EncodedPacket& pkt);

    /** @brief Whether connect() added an audio stream. */
    bool has_audio_stream() const;

    /** @brief Closes the connection and releases resources. Safe to call repeatedly. */
    void disconnect();

    /** @brief Whether connected (cleared by a failed send or disconnect()). */
    bool is_connected() const { return connected_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> connected_{false};  ///< Connection state
};

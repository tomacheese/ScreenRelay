#pragma once
#include "common/types.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief FFmpeg AAC encoder controller.
 *
 * Converts interleaved PCM from WASAPI (int16 or float32) to the encoder's required
 * format with libswresample (including resampling when the input rate or channel
 * count differs from the config) and encodes it as AAC.
 */
class AudioEncoderController {
public:
    AudioEncoderController();

    ~AudioEncoderController();

    /**
     * @brief Opens the encoder and resampler.
     * @param config          Audio settings (bitrate_kbps / sample_rate / channels are used)
     * @param in_sample_rate  Input PCM sample rate (Hz)
     * @param in_channels     Input PCM channel count
     * @param in_is_float     true for IEEE float32 input, false for int16
     * @param error           Receives an error message on failure
     * @return true on success
     */
    bool init(const AudioConfig& config, int in_sample_rate, int in_channels,
              bool in_is_float, std::string& error);

    /**
     * @brief Encodes a PCM buffer.
     *
     * Samples accumulate in an internal FIFO and are encoded once a full encoder frame is
     * available, so one call may yield zero or more packets. A buffer smaller than its
     * frame_count implies is rejected (returns false).
     *
     * @param buf         Interleaved input PCM
     * @param meta        Input metadata (capture timestamp)
     * @param out_packets Receives the encoded packets (appended)
     * @return true on success
     */
    bool encode(const AudioBuffer& buf, const AudioMeta& meta,
                std::vector<EncodedPacket>& out_packets);

    /**
     * @brief Drains packets still buffered inside the encoder.
     * @param out_packets Receives the flushed packets (appended)
     * @return true on success
     */
    bool flush(std::vector<EncodedPacket>& out_packets);

    /** @brief Releases all encoder resources; init() may be called again. */
    void reset();

    /** @brief Codec parameters (including extradata) for RTSP stream setup; empty if not initialized. */
    AudioCodecInfo get_codec_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Internal helper (audio_encoder.cpp) that encodes frames out of the FIFO
    friend bool drain_fifo_and_encode(Impl* impl, std::vector<EncodedPacket>& out_packets);
};

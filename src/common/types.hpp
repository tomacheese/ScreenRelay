#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>

/** @brief Per-monitor capture/streaming pipeline state. */
enum class PipelineState {
    INIT,           ///< Not yet initialized
    CAPTURING,      ///< Capturing, RTSP not connected yet
    CONNECTING,     ///< RTSP connecting
    STREAMING,      ///< Streaming normally
    RECONNECTING,   ///< RTSP reconnecting
    RECONFIGURING,  ///< Handling a resolution change
    STOPPING,       ///< Graceful shutdown in progress
    FATAL           ///< Unrecoverable error
};

/** @brief Name of @p s as a static string ("UNKNOWN" for out-of-range values). */
inline const char* state_name(PipelineState s) {
    switch (s) {
        case PipelineState::INIT:          return "INIT";
        case PipelineState::CAPTURING:     return "CAPTURING";
        case PipelineState::CONNECTING:    return "CONNECTING";
        case PipelineState::STREAMING:     return "STREAMING";
        case PipelineState::RECONNECTING:  return "RECONNECTING";
        case PipelineState::RECONFIGURING: return "RECONFIGURING";
        case PipelineState::STOPPING:      return "STOPPING";
        case PipelineState::FATAL:         return "FATAL";
        default:                           return "UNKNOWN";
    }
}

enum class PixelFormat { BGRA, RGBA, RGB, BGR };

/** @brief A captured frame: CPU pixel data and/or a GPU texture reference. */
struct FrameBuffer {
    std::vector<uint8_t> data;  ///< Pixel data
    uint32_t width  = 0;         ///< Width (pixels)
    uint32_t height = 0;         ///< Height (pixels)
    PixelFormat format = PixelFormat::BGRA;  ///< Pixel format

    /**
     * @brief Type-erased D3D11 texture handle for the GPU zero-copy path.
     *
     * Holds a ref-counted ID3D11Texture2D* whose custom deleter calls Release().
     * Type-erased via shared_ptr<void> to keep this common header free of D3D11.
     *
     * When set, the capture backend skipped CPU-side conversion/copies and
     * @c data may be empty; the encoder can hand the texture straight to the
     * hardware encoder. When null, the frame carries its pixels in @c data
     * (the encoder may still upload them to the GPU, see EncoderController::encode()).
     */
    std::shared_ptr<void> gpu_texture;
};

/** @brief Per-frame sequence number, timestamp and resolution. */
struct FrameMeta {
    uint64_t sequence     = 0;   ///< Frame sequence number
    int64_t  timestamp_us = 0;   ///< Timestamp (us)
    uint32_t width        = 0;   ///< Width (pixels)
    uint32_t height       = 0;   ///< Height (pixels)
};

/** @brief An FFmpeg-encoded packet (video or audio). */
struct EncodedPacket {
    std::vector<uint8_t> data;   ///< Encoded bytes
    int64_t pts         = 0;     ///< Presentation timestamp (in time_base units)
    int64_t dts         = 0;     ///< Decode timestamp (in time_base units)
    int64_t duration    = 0;     ///< Packet duration (in time_base units)
    bool is_key_frame   = false; ///< Key frame flag
    int  time_base_num  = 1;     ///< Time base numerator
    int  time_base_den  = 60;    ///< Time base denominator

    /**
     * @brief Absolute capture time of an audio packet (us, UNIX epoch).
     *
     * The audio pipeline is shared by all monitors' RTSP streams, so the packet's
     * own pts is relative to a single base (encode start). Each ScreenPipeline
     * recomputes pts from this value relative to its own RTSP connection start,
     * which keeps the RTCP SR NTP-RTP mapping consistent. Unused (0) for video.
     */
    int64_t capture_timestamp_us = 0;
};

/** @brief Video encoder settings. */
struct EncoderConfig {
    std::string codec                        = "h264_nvenc";       ///< Preferred codec
    std::vector<std::string> fallback_codecs = {"h264_mf", "libx264"};  ///< Fallback codecs, in order
    int bitrate_kbps  = 4000;      ///< Target bitrate (kbps)
    int fps           = 60;        ///< Frame rate
    int gop_size      = 60;        ///< GOP size
    int max_b_frames  = 0;         ///< Max B-frames
    std::string preset = "fast";   ///< Encoder preset
    std::string tune   = "zerolatency";  ///< Tuning option
    int threads        = 0;        ///< Encoder threads (0 = auto)
};

/** @brief Audio capture source kind. */
enum class AudioSourceKind {
    Loopback,  ///< Loopback of a render device (system playback audio)
    Capture    ///< Recording device (e.g. microphone)
};

/**
 * @brief Convert an AudioConfig::source string to AudioSourceKind.
 *
 * Shared by config validation (config_loader) and device selection
 * (audio_capture) so both apply the same rule.
 *
 * @param source "loopback" or "capture"
 * @param[out] out Result; left unchanged on failure
 * @return true if @p source is "loopback" or "capture"
 */
inline bool parse_audio_source_kind(const std::string& source, AudioSourceKind& out) {
    if (source == "loopback") { out = AudioSourceKind::Loopback; return true; }
    if (source == "capture")  { out = AudioSourceKind::Capture;  return true; }
    return false;
}

/** @brief Audio settings: whether to add audio to the streams and where to capture it from. */
struct AudioConfig {
    bool enabled            = false;       ///< Enable audio streaming
    std::string source      = "loopback";  ///< "loopback" (system playback) or "capture" (recording device)
    std::string device_id   = "";          ///< WASAPI endpoint ID; empty = default device
    int bitrate_kbps        = 128;         ///< AAC bitrate (kbps)
    int sample_rate         = 48000;       ///< Output sample rate (Hz)
    int channels            = 2;           ///< Output channel count
};

/** @brief Captured audio: interleaved PCM, IEEE float 32-bit. */
struct AudioBuffer {
    std::vector<uint8_t> data;    ///< Interleaved PCM data
    uint32_t frame_count = 0;     ///< Sample frames (one sample per channel each)
};

struct AudioMeta {
    int64_t timestamp_us = 0;  ///< Capture time (us, UNIX epoch)
};

struct AudioDeviceInfo {
    std::string id;          ///< WASAPI endpoint ID
    std::string name;        ///< Friendly name
    bool        is_render = false;  ///< true for a render (playback) device, false for a capture device
};

/** @brief Audio encoder codec info, passed to RtspPublisherClient::connect() to build the audio stream. */
struct AudioCodecInfo {
    int codec_id      = 0;      ///< AVCodecID value
    int sample_rate   = 48000;  ///< Sample rate (Hz)
    int channels      = 2;      ///< Channel count
    int bit_rate      = 0;      ///< Bitrate (bps)
    int time_base_num = 1;      ///< Time base numerator
    int time_base_den = 48000;  ///< Time base denominator (normally the sample rate)
    std::vector<uint8_t> extradata;  ///< Extra data such as AudioSpecificConfig
};

/** @brief RTSP server connection and reconnect settings. */
struct RtspConfig {
    std::string base_url;                           ///< Base URL of the RTSP server (rtsp://host:port)
    std::string path_pattern = "/live/screen{n}";   ///< Stream path pattern ({n} is replaced by the monitor number)
    int connect_timeout_ms          = 5000;          ///< Connect timeout (ms)
    int send_timeout_ms             = 5000;          ///< Send timeout (ms)
    int reconnect_delay_ms          = 1000;          ///< Initial reconnect delay (ms)
    int reconnect_max_delay_ms      = 30000;         ///< Maximum reconnect delay (ms)
    float reconnect_backoff_multiplier = 2.0f;       ///< Backoff multiplier
};

/** @brief Exponential backoff state for reconnect delays. */
struct BackoffState {
    int attempt  = 0;     ///< Attempt count
    int delay_ms = 1000;  ///< Current delay (ms)

    /** @brief Reset to zero attempts with @p initial_ms as the next delay. */
    void reset(int initial_ms = 1000) {
        attempt  = 0;
        delay_ms = initial_ms;
    }

    /**
     * @brief Return the current delay and advance: the next delay is
     *        min(delay * multiplier, max_ms).
     * @param max_ms     Delay cap (ms)
     * @param multiplier Backoff multiplier
     * @return The delay to wait now (ms)
     */
    int next_delay(int max_ms = 30000, float multiplier = 2.0f) {
        int d    = delay_ms;
        auto next = static_cast<int64_t>(
            std::min(static_cast<double>(delay_ms) * multiplier,
                     static_cast<double>(max_ms)));
        delay_ms = static_cast<int>(next);
        ++attempt;
        return d;
    }
};

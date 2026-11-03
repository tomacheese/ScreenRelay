#pragma once
#include <string>
#include <mutex>
#include <map>
#include <cstdint>

/**
 * @brief Thread-safe store of per-monitor pipeline statistics, keyed by monitor number.
 *
 * All access is serialized by a single mutex. Saved files are replaced atomically.
 * Stats entries are created on first use.
 */
class MetricsStore {
public:
    MetricsStore();

    /**
     * @brief Sets the pipeline state name.
     *
     * The health file treats "STREAMING" and "CONNECTING" as healthy and everything else as unhealthy.
     */
    void set_state(int monitor_number, const std::string& state);

    /** @brief Sets the video resolution in pixels. */
    void set_resolution(int monitor_number, int w, int h);

    /** @brief Sets the measured FPS. */
    void set_current_fps(int monitor_number, double fps);

    /** @brief Sets the measured bitrate (kbps). */
    void set_bitrate_kbps(int monitor_number, double kbps);

    /** @brief Sets the name of the codec in use. */
    void set_encoder_codec(int monitor_number, const std::string& codec);

    /** @brief Counts a received frame. */
    void increment_frames_received(int monitor_number);

    /** @brief Counts an encoded frame. */
    void increment_frames_encoded(int monitor_number);

    /** @brief Counts a dropped frame. */
    void increment_frames_dropped(int monitor_number);

    /** @brief Counts an RTSP error. */
    void increment_rtsp_errors(int monitor_number);

    /** @brief Counts a reconnect attempt. */
    void increment_reconnect_attempts(int monitor_number);

    /** @brief Resets the session start time, the baseline for uptime_ms (the constructor also sets it). */
    void mark_session_start();

    /**
     * @brief Atomically writes the metrics JSON to @p path.
     * @return true on success
     */
    bool save_metrics(const std::string& path) const;

    /**
     * @brief Atomically writes the health JSON to @p path.
     * @return true on success
     */
    bool save_health(const std::string& path) const;

private:
    /** @brief Statistics for one monitor. */
    struct MonitorStats {
        std::string state;           ///< Pipeline state name
        int width          = 0;      ///< Video width (px)
        int height         = 0;      ///< Video height (px)
        double fps         = 0.0;    ///< Current FPS
        double bitrate_kbps = 0.0;  ///< Current bitrate (kbps)
        std::string codec;           ///< Codec name
        uint64_t frames_received    = 0;  ///< Frames received
        uint64_t frames_encoded     = 0;  ///< Frames encoded
        uint64_t frames_dropped     = 0;  ///< Frames dropped
        uint64_t rtsp_errors        = 0;  ///< RTSP errors
        uint64_t reconnect_attempts = 0;  ///< Reconnect attempts
    };

    /** @brief Returns the stats entry, creating it if missing. Caller must hold mutex_. */
    MonitorStats& get_stats(int monitor_number);

    /** @brief Builds the metrics JSON (takes the lock). */
    std::string build_metrics_json() const;

    /** @brief Builds the health JSON (takes the lock). */
    std::string build_health_json() const;

    mutable std::mutex mutex_;          ///< Guards stats_ and session_start_ms_
    std::map<int, MonitorStats> stats_; ///< Monitor number -> stats
    int64_t session_start_ms_ = 0;      ///< Session start (UNIX ms)
};

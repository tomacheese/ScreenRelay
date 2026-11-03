#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief WASAPI audio capture backend.
 *
 * source == "loopback" captures what a render device (default or specified) is playing,
 * using IAudioClient loopback mode. source == "capture" reads a recording device
 * (e.g. a microphone) directly.
 *
 * A background thread pulls buffers event-driven into a bounded internal queue;
 * consumers drain it with try_pop / wait_pop.
 */
class WasapiAudioCapture {
public:
    WasapiAudioCapture();

    ~WasapiAudioCapture();

    /**
     * @brief Enumerates active audio devices (render devices for loopback, and capture devices).
     * @param error Receives an error message on failure
     * @return Device list (empty on failure)
     */
    static std::vector<AudioDeviceInfo> enumerate_devices(std::string& error);

    /**
     * @brief Opens the device and starts the capture thread.
     * @param config Audio settings (source / device_id are used)
     * @param error  Receives an error message on failure
     * @return true on success
     */
    bool init(const AudioConfig& config, std::string& error);

    /** @brief Actual device sample rate (Hz); valid after init(). */
    int sample_rate() const { return sample_rate_; }

    /** @brief Actual device channel count. */
    int channels() const { return channels_; }

    /** @brief Whether the device sample format is IEEE float (otherwise int PCM). */
    bool is_float() const { return is_float_; }

    /** @brief Bits per sample of the device format. */
    int bits_per_sample() const { return bits_per_sample_; }

    /**
     * @brief Non-blocking pop of the oldest buffer.
     * @param[out] buf  Popped buffer
     * @param[out] meta Its metadata
     * @return true if a buffer was available
     */
    bool try_pop(AudioBuffer& buf, AudioMeta& meta);

    /**
     * @brief Pops the oldest buffer, waiting up to timeout_ms.
     * @param[out] buf        Popped buffer
     * @param[out] meta       Its metadata
     * @param      timeout_ms Timeout (ms)
     * @return true if a buffer was popped, false on timeout
     */
    bool wait_pop(AudioBuffer& buf, AudioMeta& meta, int timeout_ms);

    /**
     * @brief Whether an unrecoverable capture error occurred.
     *
     * Once set, the capture thread has stopped; call init() again to recover.
     */
    bool has_hard_error() const { return hard_error_.load(); }

    /** @brief Copy of the last error message recorded by the capture thread. */
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(last_error_mutex_);
        return last_error_;
    }

    /** @brief Stops the capture thread and releases all resources. Safe to call repeatedly. */
    void release();

private:
    void capture_thread_func();

    /** Queue capacity; the oldest buffer is dropped on overflow. */
    static constexpr int kMaxQueueSize = 32;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<AudioBuffer, AudioMeta>> queue_;

    std::atomic<bool> hard_error_{false};
    mutable std::mutex last_error_mutex_;
    std::string        last_error_;

    int  sample_rate_     = 48000;
    int  channels_        = 2;
    bool is_float_        = true;
    int  bits_per_sample_ = 32;
};

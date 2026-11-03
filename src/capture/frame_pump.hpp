#pragma once
#include "common/types.hpp"
#include "capture/capture_backend.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

/**
 * @brief Pulls frames from an ICaptureBackend on a background thread into a bounded queue.
 *
 * Consumer threads take frames with try_pop / wait_pop. When the queue is
 * full, the oldest frame is dropped.
 */
class FramePump {
public:
    /** Does not start the thread; see start(). */
    FramePump();

    /** Calls stop() (joins the thread) if still running. */
    ~FramePump();

    /**
     * @brief Starts the capture thread. No-op if already running.
     *
     * @param backend          Capture backend (not owned; the caller manages its lifetime)
     * @param frame_timeout_ms Per-frame acquire timeout (ms)
     */
    void start(ICaptureBackend* backend, int frame_timeout_ms);

    /** Stops capture and joins the thread. No-op if already stopped. */
    void stop();

    /**
     * @brief Non-blocking frame retrieval.
     *
     * @param[out] buf   Frame buffer
     * @param[out] meta  Frame metadata
     * @return true if a frame was taken from the queue
     */
    bool try_pop(FrameBuffer& buf, FrameMeta& meta);

    /**
     * @brief Waits up to timeout_ms for a frame.
     *
     * Returns early (false) if the pump stops or the backend reports a hard error.
     *
     * @param[out] buf        Frame buffer
     * @param[out] meta       Frame metadata
     * @param      timeout_ms Timeout (ms)
     * @return true if a frame was taken, false on timeout/stop/error
     */
    bool wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms);

    /** @return true while the capture thread is running */
    bool is_running() const { return running_.load(); }

    /**
     * @brief Returns whether the backend reports a hard error (e.g. ACCESS_LOST).
     *
     * When true, capture must be re-initialized.
     */
    bool has_backend_error() const {
        return backend_ && backend_->has_hard_error();
    }

private:
    /** Capture thread entry point; loops until running_ becomes false. */
    void capture_thread_func();

    /** Maximum queue size; the oldest frame is dropped when full. */
    static constexpr int MAX_QUEUE_SIZE = 4;

    ICaptureBackend* backend_        = nullptr;   ///< Not owned
    int              frame_timeout_ms_ = 100;     ///< Frame acquire timeout (ms)

    std::atomic<bool>    running_{false};          ///< Thread run flag
    std::atomic<bool>    backend_hard_error_{false};  ///< Backend hard error (lets wait_pop return early)
    mutable std::mutex   mutex_;                   ///< Guards queue_
    std::condition_variable cv_;                   ///< Signals frame arrival
    std::queue<std::pair<FrameBuffer, FrameMeta>> queue_;  ///< Frame queue
    std::thread thread_;                           ///< Capture thread
    uint64_t    frame_seq_ = 0;                    ///< Frame sequence counter (capture thread only; reset in start())
};

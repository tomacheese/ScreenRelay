#include "capture/frame_pump.hpp"
#include "common/time_utils.hpp"
#include <chrono>
#include <utility>

FramePump::FramePump() = default;

FramePump::~FramePump() {
    stop();
}

void FramePump::start(ICaptureBackend* backend, int frame_timeout_ms) {
    if (running_.load()) {
        return;
    }

    if (!backend) {
        return;
    }

    backend_          = backend;
    frame_timeout_ms_ = frame_timeout_ms;
    backend_hard_error_.store(false);
    running_.store(true);

    // Drop stale frames (possibly at an old resolution) left over from a previous run.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        while (!queue_.empty()) queue_.pop();
        frame_seq_ = 0;
    }

    thread_ = std::thread(&FramePump::capture_thread_func, this);
}

void FramePump::stop() {
    running_.store(false);

    // Wake consumers blocked in wait_pop().
    cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool FramePump::try_pop(FrameBuffer& buf, FrameMeta& meta) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (queue_.empty()) {
        return false;
    }

    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();

    return true;
}

bool FramePump::wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    // Wakes on: frame available, pump stopped, or backend hard error.
    bool notified = cv_.wait_until(lk, deadline, [this]() {
        return !queue_.empty() || !running_.load() || backend_hard_error_.load();
    });

    if (!notified || queue_.empty()) {
        return false;
    }

    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();

    return true;
}

void FramePump::capture_thread_func() {
    while (running_.load()) {
        // acquire_frame() blocks up to frame_timeout_ms_, so the loop needs no extra sleep.
        auto opt = backend_->acquire_frame(frame_timeout_ms_);

        if (opt.has_value()) {
            FrameBuffer fb = std::move(*opt);

            FrameMeta meta;
            meta.sequence     = frame_seq_++;
            meta.width        = fb.width;
            meta.height       = fb.height;
            meta.timestamp_us = time_utils::system_now_us();

            {
                std::lock_guard<std::mutex> lk(mutex_);

                // Drop the oldest frame when the queue is full.
                if (static_cast<int>(queue_.size()) >= MAX_QUEUE_SIZE) {
                    queue_.pop();
                }

                queue_.emplace(std::move(fb), std::move(meta));
            }

            cv_.notify_one();
        } else if (backend_->has_hard_error()) {
            // Set the flag before notifying so the wait_pop() predicate is guaranteed to see it.
            backend_hard_error_.store(true);
            cv_.notify_all();
        }
    }
}

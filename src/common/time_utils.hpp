#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <iomanip>
#include <sstream>

/** Monotonic / wall-clock helpers and ISO 8601 formatting. */
namespace time_utils {

/** @brief Monotonic (steady_clock) time in milliseconds; the epoch is unspecified, so use only for intervals. */
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief Monotonic (steady_clock) time in microseconds; the epoch is unspecified. */
inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief Wall-clock (system_clock) time in milliseconds since the UNIX epoch. */
inline int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Wall-clock (system_clock) time in microseconds since the UNIX epoch.
 *
 * Uses the same epoch as FFmpeg's av_gettime(), so it can be compared directly
 * with RTSP start_time_realtime.
 */
inline int64_t system_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Current UTC time as ISO 8601 with milliseconds, e.g. "2026-05-01T00:00:00.000Z".
 *
 * Returns the 1970 epoch string if the UTC conversion fails.
 */
inline std::string iso8601_now() {
    auto now       = std::chrono::system_clock::now();
    auto t         = std::chrono::system_clock::to_time_t(now);
    auto ms        = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()) % 1000;
    struct tm tm_buf{};
    bool time_ok = true;
#if defined(_WIN32)
    time_ok = (gmtime_s(&tm_buf, &t) == 0);
#else
    time_ok = (gmtime_r(&t, &tm_buf) != nullptr);
#endif
    if (!time_ok) return "1970-01-01T00:00:00.000Z";
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

/** @brief Elapsed-time measurement on steady_clock, started at construction or reset(). */
class Stopwatch {
public:
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}

    void reset() { start_ = std::chrono::steady_clock::now(); }

    int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

    int64_t elapsed_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

    /** @brief True once at least @p timeout_ms milliseconds have elapsed. */
    bool expired(int64_t timeout_ms) const {
        return elapsed_ms() >= timeout_ms;
    }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace time_utils

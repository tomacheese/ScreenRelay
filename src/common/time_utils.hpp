#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <iomanip>
#include <sstream>

/**
 * @brief 時刻・タイマーユーティリティ
 *
 * 単調時計・システム時計の取得と ISO 8601 文字列変換を提供する。
 */
namespace time_utils {

/**
 * @brief 単調時計の現在時刻をミリ秒で返す
 * @return ミリ秒単位の時刻 (エポック起点)
 */
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief 単調時計の現在時刻をマイクロ秒で返す
 * @return マイクロ秒単位の時刻 (エポック起点)
 */
inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief システム時計の現在時刻をミリ秒で返す
 * @return ミリ秒単位の時刻 (UNIX エポック起点)
 */
inline int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief システム時計の現在時刻をマイクロ秒で返す
 *
 * FFmpeg の av_gettime() と同一のエポック (UNIX エポック) を使用するため、
 * RTSP の start_time_realtime との比較・計算に直接利用できる。
 *
 * @return マイクロ秒単位の時刻 (UNIX エポック起点)
 */
inline int64_t system_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief 現在時刻を ISO 8601 形式の文字列で返す
 *
 * 例: "2026-05-01T00:00:00.000Z"
 * @return ISO 8601 形式のタイムスタンプ文字列
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

/**
 * @brief 経過時間計測クラス
 *
 * 生成時点からの経過時間をミリ秒・マイクロ秒で取得できる。
 */
class Stopwatch {
public:
    /** @brief コンストラクタ。計測開始時刻を記録する。 */
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}

    /** @brief 計測開始時刻をリセットする */
    void reset() { start_ = std::chrono::steady_clock::now(); }

    /**
     * @brief 経過時間をミリ秒で返す
     * @return 経過ミリ秒
     */
    int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

    /**
     * @brief 経過時間をマイクロ秒で返す
     * @return 経過マイクロ秒
     */
    int64_t elapsed_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

    /**
     * @brief 指定したタイムアウト時間が経過しているか判定する
     * @param timeout_ms タイムアウト時間 (ms)
     * @return タイムアウトしていれば true
     */
    bool expired(int64_t timeout_ms) const {
        return elapsed_ms() >= timeout_ms;
    }

private:
    std::chrono::steady_clock::time_point start_;  ///< 計測開始時刻
};

} // namespace time_utils

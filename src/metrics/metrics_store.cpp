#include "metrics/metrics_store.hpp"
#include "common/time_utils.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

MetricsStore::MetricsStore() = default;

MetricsStore::MonitorStats& MetricsStore::get_stats(int monitor_number) {
    // マップに存在しない場合はデフォルト構築される
    return stats_[monitor_number];
}

void MetricsStore::set_state(int monitor_number, const std::string& state) {
    std::lock_guard<std::mutex> lk(mutex_);
    get_stats(monitor_number).state = state;
}

void MetricsStore::set_resolution(int monitor_number, int w, int h) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto& s = get_stats(monitor_number);
    s.width  = w;
    s.height = h;
}

void MetricsStore::set_current_fps(int monitor_number, double fps) {
    std::lock_guard<std::mutex> lk(mutex_);
    get_stats(monitor_number).fps = fps;
}

void MetricsStore::set_bitrate_kbps(int monitor_number, double kbps) {
    std::lock_guard<std::mutex> lk(mutex_);
    get_stats(monitor_number).bitrate_kbps = kbps;
}

void MetricsStore::set_encoder_codec(int monitor_number, const std::string& codec) {
    std::lock_guard<std::mutex> lk(mutex_);
    get_stats(monitor_number).codec = codec;
}

void MetricsStore::increment_frames_received(int monitor_number) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++get_stats(monitor_number).frames_received;
}

void MetricsStore::increment_frames_encoded(int monitor_number) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++get_stats(monitor_number).frames_encoded;
}

void MetricsStore::increment_frames_dropped(int monitor_number) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++get_stats(monitor_number).frames_dropped;
}

void MetricsStore::increment_rtsp_errors(int monitor_number) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++get_stats(monitor_number).rtsp_errors;
}

void MetricsStore::increment_reconnect_attempts(int monitor_number) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++get_stats(monitor_number).reconnect_attempts;
}

void MetricsStore::mark_session_start() {
    std::lock_guard<std::mutex> lk(mutex_);
    session_start_ms_ = time_utils::system_now_ms();
}

std::string MetricsStore::build_metrics_json() const {
    json j;
    j["ts"] = time_utils::iso8601_now();

    {
        std::lock_guard<std::mutex> lk(mutex_);
        int64_t uptime_ms = time_utils::system_now_ms() - session_start_ms_;
        j["uptime_ms"] = uptime_ms;

        // モニターごとのデータを "monitors" オブジェクトに格納する
        json monitors = json::object();
        for (const auto& [monitor_number, s] : stats_) {
            json m;
            m["state"]              = s.state;
            m["width"]              = s.width;
            m["height"]             = s.height;
            m["fps"]                = s.fps;
            m["bitrate_kbps"]       = s.bitrate_kbps;
            m["codec"]              = s.codec;
            m["frames_received"]    = s.frames_received;
            m["frames_encoded"]     = s.frames_encoded;
            m["frames_dropped"]     = s.frames_dropped;
            m["rtsp_errors"]        = s.rtsp_errors;
            m["reconnect_attempts"] = s.reconnect_attempts;
            monitors[std::to_string(monitor_number)] = m;
        }
        j["monitors"] = monitors;
    }

    return j.dump(2);
}

std::string MetricsStore::build_health_json() const {
    json j;
    j["ts"] = time_utils::iso8601_now();

    {
        std::lock_guard<std::mutex> lk(mutex_);

        // モニターごとのヘルス状態を "monitors" オブジェクトに格納し、
        // 全モニターが健全かどうかをトップレベルの "healthy" に反映する
        bool overall_healthy = !stats_.empty();
        json monitors = json::object();
        for (const auto& [monitor_number, s] : stats_) {
            // "STREAMING" または "CONNECTING" 状態なら healthy とみなす
            bool healthy = (s.state == "STREAMING" || s.state == "CONNECTING");
            if (!healthy) overall_healthy = false;
            json m;
            m["healthy"] = healthy;
            m["state"]   = s.state;
            monitors[std::to_string(monitor_number)] = m;
        }
        j["healthy"]  = overall_healthy;
        j["monitors"] = monitors;
    }

    return j.dump(2);
}

/**
 * @brief ファイルをアトミックに書き込む
 *
 * 一時ファイルに書き込んだ後にリネームすることで
 * 書き込み途中の読み取りを防ぐ。
 * @param path    書き込み先パス
 * @param content 書き込む内容
 * @return 成功時 true
 */
static bool write_atomic(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    try {
        auto parent = fs::path(path).parent_path();
        if (!parent.empty())
            fs::create_directories(parent);
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << content;
        f.close();
        // Windows では fs::rename が既存ファイルを上書きしないため
        // 先に削除してからリネームする
        if (fs::exists(path))
            fs::remove(path);
        fs::rename(tmp, path);
        return true;
    } catch (...) {
        return false;
    }
}

bool MetricsStore::save_metrics(const std::string& path) const {
    if (!write_atomic(path, build_metrics_json())) {
        fprintf(stderr, "[ERROR] Failed to write metrics to: %s\n", path.c_str());
        return false;
    }
    return true;
}

bool MetricsStore::save_health(const std::string& path) const {
    if (!write_atomic(path, build_health_json())) {
        fprintf(stderr, "[ERROR] Failed to write health to: %s\n", path.c_str());
        return false;
    }
    return true;
}

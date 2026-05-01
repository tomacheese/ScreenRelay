#include "logging/log_sink.hpp"
#include "common/time_utils.hpp"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

LogSink::LogSink() = default;

LogSink::~LogSink() {
    if (logger_) logger_->flush();
}

bool LogSink::init(const std::string& log_dir,
                   const std::string& instance_name,
                   const std::string& log_level,
                   std::string& error) {
    try {
        // ログディレクトリを作成する
        fs::create_directories(log_dir);

        std::string log_path = log_dir + "/" + instance_name + ".jsonl";

        // ローテーティングファイルシンク (最大 10 MB × 5 ファイル)
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, 10 * 1024 * 1024 /* 10 MB */, 5 /* files */);

        // コンソールシンク
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        logger_ = std::make_shared<spdlog::logger>(
            instance_name,
            spdlog::sinks_init_list{file_sink, console_sink});

        // JSON は手動で構築するためパターンはメッセージのみ
        logger_->set_pattern("%v");

        // ログレベルを文字列から spdlog::level に変換して設定する
        spdlog::level::level_enum level = spdlog::level::info;
        if      (log_level == "trace")    level = spdlog::level::trace;
        else if (log_level == "debug")    level = spdlog::level::debug;
        else if (log_level == "info")     level = spdlog::level::info;
        else if (log_level == "warn")     level = spdlog::level::warn;
        else if (log_level == "error")    level = spdlog::level::err;
        else if (log_level == "critical") level = spdlog::level::critical;
        // 不明なレベルはデフォルトの info を使用する

        logger_->set_level(level);

    } catch (const std::exception& e) {
        error = std::string("Failed to init log sink: ") + e.what();
        return false;
    }

    return true;
}

/**
 * @brief JSON の文字列値をエスケープする
 * @param s エスケープ対象の文字列
 * @return エスケープ済み文字列
 */
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // JSON 仕様に従い制御文字を \uXXXX でエスケープする
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string LogSink::build_json(const std::string& event_name,
                                const std::map<std::string, std::string>& fields) {
    std::ostringstream oss;
    oss << "{\"ts\":\"" << time_utils::iso8601_now() << "\"";
    oss << ",\"event\":\"" << json_escape(event_name) << "\"";
    for (const auto& kv : fields) {
        oss << ",\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

void LogSink::log_event(spdlog::level::level_enum level,
                        const std::string& event_name,
                        const std::map<std::string, std::string>& fields) {
    if (!logger_) return;
    logger_->log(level, build_json(event_name, fields));
}

void LogSink::log_state_changed(int monitor_number,
                                const std::string& from, const std::string& to) {
    log_event(spdlog::level::info, "state_changed",
              {{"monitor", std::to_string(monitor_number)},
               {"from",    from},
               {"to",      to}});
}

void LogSink::log_error(const std::string& code,
                        const std::string& message,
                        const std::map<std::string, std::string>& extra) {
    std::map<std::string, std::string> fields = {{"code", code}, {"message", message}};
    for (const auto& kv : extra) fields[kv.first] = kv.second;
    log_event(spdlog::level::err, "error", fields);
}

void LogSink::log_publish_started(int monitor_number, const std::string& url,
                                  int width, int height, int fps) {
    log_event(spdlog::level::info, "publish_started",
              {{"monitor", std::to_string(monitor_number)},
               {"url",     url},
               {"width",   std::to_string(width)},
               {"height",  std::to_string(height)},
               {"fps",     std::to_string(fps)}});
}

void LogSink::log_publish_stopped(int monitor_number, const std::string& reason) {
    log_event(spdlog::level::info, "publish_stopped",
              {{"monitor", std::to_string(monitor_number)},
               {"reason",  reason}});
}

void LogSink::log_reconnect(int monitor_number, int attempt, int delay_ms) {
    log_event(spdlog::level::warn, "reconnecting",
              {{"monitor",  std::to_string(monitor_number)},
               {"attempt",  std::to_string(attempt)},
               {"delay_ms", std::to_string(delay_ms)}});
}

void LogSink::log_encoder_initialized(int monitor_number, const std::string& codec,
                                      int width, int height, int fps, int bitrate_kbps) {
    log_event(spdlog::level::info, "encoder_initialized",
              {{"monitor",       std::to_string(monitor_number)},
               {"codec",         codec},
               {"width",         std::to_string(width)},
               {"height",        std::to_string(height)},
               {"fps",           std::to_string(fps)},
               {"bitrate_kbps",  std::to_string(bitrate_kbps)}});
}

void LogSink::log_monitor_added(int monitor_number, const std::string& device_name,
                                int width, int height, bool is_primary) {
    log_event(spdlog::level::info, "monitor_added",
              {{"monitor",    std::to_string(monitor_number)},
               {"device",     device_name},
               {"width",      std::to_string(width)},
               {"height",     std::to_string(height)},
               {"is_primary", is_primary ? "true" : "false"}});
}

void LogSink::log_monitor_removed(int monitor_number) {
    log_event(spdlog::level::info, "monitor_removed",
              {{"monitor", std::to_string(monitor_number)}});
}

void LogSink::flush() {
    if (logger_) logger_->flush();
}

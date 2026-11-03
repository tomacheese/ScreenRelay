#pragma once
#include <string>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

/**
 * @brief JSON Lines log sink writing to a rotating file and the console.
 *
 * Every line is one JSON object with "ts" and "event" plus event-specific
 * string fields. All fields are emitted as JSON strings, including numbers.
 * Calls are no-ops until init() succeeds.
 */
class LogSink {
public:
    LogSink();

    /** @brief Flushes buffered output. */
    ~LogSink();

    /**
     * @brief Create the logger: <log_dir>/<instance_name>.jsonl (size-rotated) plus console.
     * @param log_dir       Log directory (created if missing)
     * @param instance_name Instance name (log file base name)
     * @param log_level     trace/debug/info/warn/error/critical; unknown values fall back to info
     * @param[out] error    Message on failure
     * @return true on success
     */
    bool init(const std::string& log_dir,
              const std::string& instance_name,
              const std::string& log_level,
              std::string& error);

    /**
     * @brief Log an arbitrary event.
     * @param level      Log level
     * @param event_name Value of the "event" field
     * @param fields     Extra fields
     */
    void log_event(spdlog::level::level_enum level,
                   const std::string& event_name,
                   const std::map<std::string, std::string>& fields = {});

    /** @brief Log a "state_changed" event (state names as from state_name()). */
    void log_state_changed(int monitor_number,
                           const std::string& from, const std::string& to);

    /**
     * @brief Log an "error" event.
     * @param code    Error code (use the errors:: constants)
     * @param message Error message
     * @param extra   Extra fields; on a key clash they override "code"/"message"
     */
    void log_error(const std::string& code,
                   const std::string& message,
                   const std::map<std::string, std::string>& extra = {});

    /** @brief Log a "publish_started" event. */
    void log_publish_started(int monitor_number, const std::string& url,
                             int width, int height, int fps);

    /** @brief Log a "publish_stopped" event. */
    void log_publish_stopped(int monitor_number, const std::string& reason);

    /** @brief Log a "reconnecting" event (warn); @p delay_ms is the wait before the next attempt. */
    void log_reconnect(int monitor_number, int attempt, int delay_ms);

    /** @brief Log an "encoder_initialized" event. */
    void log_encoder_initialized(int monitor_number, const std::string& codec,
                                 int width, int height, int fps, int bitrate_kbps);

    /** @brief Log a "monitor_added" event. */
    void log_monitor_added(int monitor_number, const std::string& device_name,
                           int width, int height, bool is_primary);

    /** @brief Log a "monitor_removed" event. */
    void log_monitor_removed(int monitor_number);

    /** @brief Flush buffered output. */
    void flush();

private:
    /** @brief Build one JSON line: {"ts", "event", ...fields} (all values as strings). */
    std::string build_json(const std::string& event_name,
                           const std::map<std::string, std::string>& fields);

    std::shared_ptr<spdlog::logger> logger_;
};

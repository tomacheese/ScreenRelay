#pragma once
#include "common/types.hpp"
#include <string>

/**
 * @brief Application settings loaded from the JSON config file, one member per section.
 *
 * Defaults are the field initializers; keys missing from the file keep them.
 */
struct AppConfig {
    struct App {
        std::string instance_name = "screen-relay";          ///< Instance name (used for the log file name)
        std::string log_dir       = "./logs";                 ///< Log output directory
        std::string log_level     = "info";                   ///< Log level (trace/debug/info/warn/error/critical)
        std::string metrics_path  = "./state/metrics.json";  ///< Metrics output file path
        std::string health_path   = "./state/health.json";   ///< Health check output file path
    } app;

    struct Capture {
        std::string backend       = "dxgi";  ///< Capture backend (only "dxgi" is supported)
        int frame_timeout_ms      = 100;     ///< Frame acquisition timeout (ms); 0 returns immediately
    } capture;

    EncoderConfig encoder;  ///< Encoder settings
    RtspConfig    rtsp;     ///< RTSP settings
    AudioConfig   audio;    ///< Audio settings (disabled by default)

    struct Runtime {
        int monitor_check_interval_ms = 1000;  ///< Monitor connect/disconnect check interval (ms)
        int shutdown_grace_ms         = 3000;  ///< Graceful shutdown wait (ms)
        int emit_metrics_interval_ms  = 1000;  ///< Metrics write interval (ms)
    } runtime;
};

/** @brief Loads and validates the JSON config file into AppConfig. */
class ConfigLoader {
public:
    /**
     * @brief Read and validate a JSON config file.
     * @param path  Config file path
     * @param[in,out] out Config to fill; keys absent from the file keep the values already in @p out
     * @param[out] error Message on failure (open, parse, type or validation error)
     * @return true on success
     */
    static bool load(const std::string& path, AppConfig& out, std::string& error);
};

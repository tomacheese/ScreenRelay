#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <atomic>
#include "app/supervisor.hpp"
#include "audio/audio_capture.hpp"
#include "config/config_loader.hpp"

/** Set by the console handler to record that a stop was requested. */
static std::atomic<bool> g_stop_requested{false};

/** Lets the console handler (runs on a separate OS thread) reach the supervisor. */
static std::atomic<MonitorSupervisor*> g_supervisor{nullptr};

/**
 * @brief Console control handler (Ctrl+C, Ctrl+Break, console close).
 *
 * Requests a graceful stop on the supervisor.
 *
 * @param ctrl_type Control event type
 * @return TRUE if handled, FALSE to fall through to the next handler
 */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true);
        auto* sv = g_supervisor.load();
        if (sv) sv->request_stop();
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Entry point: parse arguments, load the config and run MonitorSupervisor.
 *
 * @return 0 on success, 1 on error
 */
int main(int argc, char* argv[]) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--list-audio-devices") {
            std::string enum_error;
            auto devices = WasapiAudioCapture::enumerate_devices(enum_error);
            if (!enum_error.empty()) {
                std::cerr << "[ERROR] " << enum_error << "\n";
                return 1;
            }
            for (const auto& d : devices) {
                std::cout << (d.is_render ? "[loopback] " : "[capture]  ")
                          << d.id << "  " << d.name << "\n";
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: " << argv[0]
                      << " [--config <path>] [--list-audio-devices]\n";
            return 0;
        }
    }

    if (config_path.empty()) config_path = "config/config.json";

    AppConfig config;
    std::string load_error;
    if (!ConfigLoader::load(config_path, config, load_error)) {
        std::cerr << "[ERROR] " << load_error << "\n";
        return 1;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    MonitorSupervisor supervisor;
    g_supervisor.store(&supervisor);

    std::string init_error;
    if (!supervisor.init(config, init_error)) {
        std::cerr << "[ERROR] Supervisor init failed: " << init_error << "\n";
        return 1;
    }

    supervisor.run();

    g_supervisor.store(nullptr);
    return 0;
}

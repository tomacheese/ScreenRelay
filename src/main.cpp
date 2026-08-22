#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <atomic>
#include "app/supervisor.hpp"
#include "audio/audio_capture.hpp"
#include "config/config_loader.hpp"

/** @brief グローバル停止フラグ。コンソールハンドラーから設定される */
static std::atomic<bool> g_stop_requested{false};

/** @brief グローバルスーパーバイザーポインタ。コンソールハンドラーから参照する */
static std::atomic<MonitorSupervisor*> g_supervisor{nullptr};

/**
 * @brief Windows コンソールイベントハンドラー
 *
 * Ctrl+C、Ctrl+Break、ウィンドウクローズのいずれかが発生した場合に
 * スーパーバイザーへ停止リクエストを送る。
 *
 * @param ctrl_type コントロールイベントの種別
 * @return 処理した場合 TRUE、それ以外の場合 FALSE
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
 * @brief アプリケーションエントリポイント
 *
 * 引数を解析して設定ファイルをロードし、MonitorSupervisor を起動する。
 * コンソールイベントハンドラーを登録してグレースフルシャットダウンをサポートする。
 *
 * @param argc コマンドライン引数の数
 * @param argv コマンドライン引数の配列
 * @return 成功時 0、エラー時 1
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

    // デフォルト設定ファイルパス
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

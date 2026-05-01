#include <cstdio>
#include <fstream>
#include <filesystem>
#include "config/config_loader.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;

/**
 * @brief テスト用一時設定ファイルを書き出す
 * @param content JSON 文字列
 * @return 書き出したファイルパス
 */
static std::string write_temp_config(const std::string& content) {
    std::string path = "test_config_tmp.json";
    std::ofstream f(path);
    f << content;
    return path;
}

/**
 * @brief テスト用一時ファイルを削除する
 * @param path 削除するファイルパス
 */
static void cleanup(const std::string& path) {
    fs::remove(path);
}

/** @brief 最小限の有効な設定ファイルが読み込めること */
static void test_load_minimal_valid() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Minimal valid config should load");
    VERIFY(cfg.rtsp.base_url == "rtsp://192.168.0.100:8554");
    VERIFY(cfg.encoder.fps == 60);
    VERIFY(cfg.encoder.bitrate_kbps == 4000);
    printf("[PASS] test_load_minimal_valid\n");
}

/** @brief 全セクションを含む設定ファイルが読み込めること */
static void test_load_all_sections() {
    const std::string json = R"({
      "app": {
        "instance_name": "test-01",
        "log_dir": "./testlogs",
        "metrics_path": "./state/m.json",
        "health_path": "./state/h.json"
      },
      "capture": {
        "backend": "wgc",
        "frame_timeout_ms": 200
      },
      "encoder": {
        "codec": "libx264",
        "fallback_codecs": ["h264_mf", "libx264"],
        "bitrate_kbps": 2000,
        "fps": 30,
        "gop_size": 30,
        "max_b_frames": 0,
        "preset": "ultrafast",
        "tune": "zerolatency"
      },
      "rtsp": {
        "base_url": "rtsp://localhost:8554",
        "path_pattern": "/live/screen{n}",
        "connect_timeout_ms": 3000,
        "reconnect_delay_ms": 500
      },
      "runtime": {
        "shutdown_grace_ms": 2000,
        "emit_metrics_interval_ms": 500
      }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Full config should load");
    VERIFY(cfg.app.instance_name == "test-01");
    VERIFY(cfg.capture.backend == "wgc");
    VERIFY(cfg.encoder.codec == "libx264");
    VERIFY(cfg.encoder.fps == 30);
    // fallback_codecs が正しく読み込まれること
    VERIFY(cfg.encoder.fallback_codecs.size() == 2);
    VERIFY(cfg.encoder.fallback_codecs[0] == "h264_mf");
    VERIFY(cfg.encoder.fallback_codecs[1] == "libx264");
    VERIFY(cfg.rtsp.base_url == "rtsp://localhost:8554");
    VERIFY(cfg.runtime.shutdown_grace_ms == 2000);
    printf("[PASS] test_load_all_sections\n");
}

/** @brief ファイルが存在しない場合にエラーになること */
static void test_missing_file() {
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load("nonexistent_file_xyz.json", cfg, err);
    VERIFY_MSG(!ok, "Missing file should fail");
    VERIFY(!err.empty());
    printf("[PASS] test_missing_file\n");
}

/** @brief 不正な JSON でエラーになること */
static void test_invalid_json() {
    auto path = write_temp_config("{invalid json}");
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Invalid JSON should fail");
    printf("[PASS] test_invalid_json\n");
}

/** @brief rtsp.base_url がない場合にエラーになること */
static void test_missing_rtsp_base_url_fails() {
    const std::string json = R"({
      "encoder": { "fps": 60, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Missing rtsp.base_url should fail validation");
    printf("[PASS] test_missing_rtsp_base_url_fails\n");
}

/** @brief fps が範囲外でエラーになること */
static void test_invalid_fps_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://host:8554" },
      "encoder": { "fps": -1, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Negative fps should fail");
    printf("[PASS] test_invalid_fps_fails\n");
}

/** @brief base_url が "rtsp://" で始まらない場合にエラーになること */
static void test_invalid_rtsp_url_prefix_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "http://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Non-rtsp:// URL should fail validation");
    printf("[PASS] test_invalid_rtsp_url_prefix_fails\n");
}

/**
 * @brief ConfigLoader のユニットテストを実行する
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_config_loader_tests() {
    printf("=== Config Loader Tests ===\n");
    test_load_minimal_valid();
    test_load_all_sections();
    test_missing_file();
    test_invalid_json();
    test_missing_rtsp_base_url_fails();
    test_invalid_fps_fails();
    test_invalid_rtsp_url_prefix_fails();
    return 0;
}

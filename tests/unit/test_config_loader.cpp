#include <cstdio>
#include <fstream>
#include <filesystem>
#include "config/config_loader.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;

// Writes a temporary config file (in the working directory) and returns its path.
static std::string write_temp_config(const std::string& content) {
    std::string path = "test_config_tmp.json";
    std::ofstream f(path);
    f << content;
    return path;
}

static void cleanup(const std::string& path) {
    fs::remove(path);
}

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

static void test_load_all_sections() {
    const std::string json = R"({
      "app": {
        "instance_name": "test-01",
        "log_dir": "./testlogs",
        "metrics_path": "./state/m.json",
        "health_path": "./state/h.json"
      },
      "capture": {
        "backend": "dxgi",
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
    VERIFY(cfg.capture.backend == "dxgi");
    VERIFY(cfg.encoder.codec == "libx264");
    VERIFY(cfg.encoder.fps == 30);
    VERIFY(cfg.encoder.fallback_codecs.size() == 2);
    VERIFY(cfg.encoder.fallback_codecs[0] == "h264_mf");
    VERIFY(cfg.encoder.fallback_codecs[1] == "libx264");
    VERIFY(cfg.rtsp.base_url == "rtsp://localhost:8554");
    VERIFY(cfg.runtime.shutdown_grace_ms == 2000);
    printf("[PASS] test_load_all_sections\n");
}

static void test_missing_file() {
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load("nonexistent_file_xyz.json", cfg, err);
    VERIFY_MSG(!ok, "Missing file should fail");
    VERIFY(!err.empty());
    printf("[PASS] test_missing_file\n");
}

static void test_invalid_json() {
    auto path = write_temp_config("{invalid json}");
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Invalid JSON should fail");
    printf("[PASS] test_invalid_json\n");
}

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

// "wgc" is not a supported capture backend
static void test_invalid_backend_fails() {
    const std::string json = R"({
      "capture": { "backend": "wgc" },
      "rtsp": { "base_url": "rtsp://host:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Unsupported backend should fail");
    printf("[PASS] test_invalid_backend_fails\n");
}

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

// A missing audio section loads with audio disabled
static void test_audio_default_disabled() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Config without audio section should still load");
    VERIFY(cfg.audio.enabled == false);
    printf("[PASS] test_audio_default_disabled\n");
}

static void test_audio_parses_all_fields() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": {
        "enabled": true,
        "source": "capture",
        "device_id": "{device-guid}",
        "bitrate_kbps": 160,
        "sample_rate": 44100,
        "channels": 1
      }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Config with audio section should load");
    VERIFY(cfg.audio.enabled == true);
    VERIFY(cfg.audio.source == "capture");
    VERIFY(cfg.audio.device_id == "{device-guid}");
    VERIFY(cfg.audio.bitrate_kbps == 160);
    VERIFY(cfg.audio.sample_rate == 44100);
    VERIFY(cfg.audio.channels == 1);
    printf("[PASS] test_audio_parses_all_fields\n");
}

static void test_audio_invalid_source_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": { "enabled": true, "source": "microphone" }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Unsupported audio.source should fail validation");
    printf("[PASS] test_audio_invalid_source_fails\n");
}

static void test_audio_invalid_channels_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": { "enabled": true, "channels": 6 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "audio.channels other than 1/2 should fail validation");
    printf("[PASS] test_audio_invalid_channels_fails\n");
}

static void test_audio_invalid_bitrate_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": { "enabled": true, "bitrate_kbps": 0 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "audio.bitrate_kbps <= 0 should fail validation");
    printf("[PASS] test_audio_invalid_bitrate_fails\n");
}

static void test_audio_invalid_sample_rate_fails() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": { "enabled": true, "sample_rate": 0 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "audio.sample_rate <= 0 should fail validation");
    printf("[PASS] test_audio_invalid_sample_rate_fails\n");
}

// Invalid audio values are ignored while audio.enabled is false
static void test_audio_disabled_skips_validation() {
    const std::string json = R"({
      "rtsp": { "base_url": "rtsp://192.168.0.100:8554" },
      "encoder": { "fps": 60, "bitrate_kbps": 4000 },
      "audio": { "enabled": false, "source": "microphone", "channels": 6 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Disabled audio section should skip validation");
    printf("[PASS] test_audio_disabled_skips_validation\n");
}

/**
 * @brief Tests ConfigLoader.
 * @return 0 on success (a failing VERIFY exits with status 1)
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
    test_invalid_backend_fails();
    test_audio_default_disabled();
    test_audio_parses_all_fields();
    test_audio_invalid_source_fails();
    test_audio_invalid_channels_fails();
    test_audio_invalid_bitrate_fails();
    test_audio_invalid_sample_rate_fails();
    test_audio_disabled_skips_validation();
    return 0;
}

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "metrics/metrics_store.hpp"
#include "test_utils.hpp"

// nlohmann/json は screen_relay_lib 経由でリンクされる
#include <nlohmann/json.hpp>

/** @brief スコープ終了時にファイルを削除する RAII ガード */
struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() { std::remove(path.c_str()); }
};

/** @brief モニター 1 のメトリクスを設定して JSON に保存できること */
static void test_set_and_save_metrics() {
    MetricsStore ms;
    ms.mark_session_start();
    ms.set_state(1, "STREAMING");
    ms.set_resolution(1, 1920, 1080);
    ms.set_current_fps(1, 59.9);
    ms.set_bitrate_kbps(1, 4000.0);
    ms.set_encoder_codec(1, "h264_nvenc");
    ms.increment_frames_received(1);
    ms.increment_frames_encoded(1);

    std::string path = "test_metrics_tmp.json";
    TempFileGuard guard(path);
    bool ok = ms.save_metrics(path);
    VERIFY_MSG(ok, "save_metrics should succeed");

    // ファイルが存在すること
    std::ifstream chk(path);
    VERIFY_MSG(chk.is_open(), "metrics file should exist");
    chk.close();

    // JSON として解析できること
    std::ifstream f(path);
    nlohmann::json j;
    f >> j;

    // 保存内容に期待するフィールドが含まれること
    VERIFY_MSG(j.contains("monitors"), "JSON should contain 'monitors'");

    printf("[PASS] test_set_and_save_metrics\n");
}

/** @brief モニター 1 と 2 の両方のメトリクスが正しく保存されること */
static void test_multiple_monitors() {
    MetricsStore ms;
    ms.set_state(1, "STREAMING");
    ms.set_resolution(1, 1920, 1080);
    ms.set_state(2, "CONNECTING");
    ms.set_resolution(2, 2560, 1440);

    std::string path = "test_metrics_multi_tmp.json";
    TempFileGuard guard(path);
    bool ok = ms.save_metrics(path);
    VERIFY_MSG(ok, "save_metrics for multiple monitors should succeed");

    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    f.close();

    // モニター 1 と 2 の両方が存在すること
    VERIFY_MSG(j.contains("monitors"), "JSON should contain 'monitors'");
    auto& monitors = j["monitors"];
    // キーは文字列化されたモニター番号の可能性があるため、
    // 少なくとも 2 エントリが存在することを確認する
    VERIFY_MSG(monitors.size() >= 2, "Should have at least 2 monitor entries");

    printf("[PASS] test_multiple_monitors\n");
}

/** @brief STREAMING 状態なら healthy=true であること */
static void test_health_streaming_is_healthy() {
    MetricsStore ms;
    ms.set_state(1, "STREAMING");

    std::string path = "test_health_streaming_tmp.json";
    TempFileGuard guard(path);
    bool ok = ms.save_health(path);
    VERIFY_MSG(ok, "save_health should succeed");

    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    f.close();

    VERIFY_MSG(j.contains("healthy"), "JSON should contain 'healthy'");
    VERIFY_MSG(j["healthy"] == true, "STREAMING state should be healthy");

    printf("[PASS] test_health_streaming_is_healthy\n");
}

/** @brief FATAL 状態なら healthy=false であること */
static void test_health_fatal_is_unhealthy() {
    MetricsStore ms;
    ms.set_state(1, "FATAL");

    std::string path = "test_health_fatal_tmp.json";
    TempFileGuard guard(path);
    ms.save_health(path);

    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    f.close();

    VERIFY_MSG(j.contains("healthy"), "JSON should contain 'healthy'");
    VERIFY_MSG(j["healthy"] == false, "FATAL state should not be healthy");

    printf("[PASS] test_health_fatal_is_unhealthy\n");
}

/** @brief increment_* が正しくカウントを増加させること */
static void test_counters() {
    MetricsStore ms;
    // 初期値の確認はファイル保存 JSON で行う
    ms.increment_frames_received(1);
    ms.increment_frames_received(1);
    ms.increment_frames_encoded(1);
    ms.increment_frames_dropped(1);
    ms.increment_rtsp_errors(1);
    ms.increment_reconnect_attempts(1);

    std::string path = "test_metrics_counters_tmp.json";
    TempFileGuard guard(path);
    bool ok = ms.save_metrics(path);
    VERIFY_MSG(ok, "save_metrics should succeed");

    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    f.close();

    // モニター 1 のカウンターを検証する
    VERIFY_MSG(j.contains("monitors"), "JSON should contain 'monitors'");

    // monitors オブジェクト内で最初のエントリを取得してカウンターを確認する
    bool found = false;
    for (auto& [key, val] : j["monitors"].items()) {
        if (val.contains("frames_received")) {
            VERIFY_MSG(val["frames_received"] == 2,  "frames_received should be 2");
            VERIFY_MSG(val["frames_encoded"]  == 1,  "frames_encoded should be 1");
            VERIFY_MSG(val["frames_dropped"]  == 1,  "frames_dropped should be 1");
            VERIFY_MSG(val["rtsp_errors"]     == 1,  "rtsp_errors should be 1");
            VERIFY_MSG(val["reconnect_attempts"] == 1, "reconnect_attempts should be 1");
            found = true;
            break;
        }
    }
    VERIFY_MSG(found, "Monitor 1 stats should be present in JSON");

    printf("[PASS] test_counters\n");
}

/**
 * @brief MetricsStore のユニットテストを実行する
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_metrics_tests() {
    printf("=== Metrics Store Tests ===\n");
    test_set_and_save_metrics();
    test_multiple_monitors();
    test_health_streaming_is_healthy();
    test_health_fatal_is_unhealthy();
    test_counters();
    return 0;
}

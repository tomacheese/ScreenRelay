#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace fs = std::filesystem;

/** @brief スコープ終了時にディレクトリを再帰的に削除する RAII ガード */
struct DirGuard {
    std::string path;
    explicit DirGuard(std::string p) : path(std::move(p)) {}
    ~DirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
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

/** @brief 既存ファイルを繰り返し置換しても常に有効な JSON が読め、一時ファイルが残らないこと */
static void test_save_replaces_existing_file() {
    MetricsStore ms;
    ms.set_state(1, "STREAMING");

    std::string path = "test_metrics_replace_tmp.json";
    TempFileGuard guard(path);
    TempFileGuard tmp_guard(path + ".tmp");

    for (int i = 0; i < 20; ++i) {
        ms.increment_frames_received(1);
        VERIFY_MSG(ms.save_metrics(path), "save_metrics should succeed when file exists");

        std::ifstream f(path);
        VERIFY_MSG(f.is_open(), "metrics file should exist after replace");
        // 空や途中までのファイルでも例外ではなく VERIFY_MSG で失敗を報告する
        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        VERIFY_MSG(!j.is_discarded(), "metrics file should be valid JSON after replace");
        VERIFY_MSG(j["monitors"]["1"]["frames_received"] == i + 1,
                   "metrics file should hold the latest content");

        std::ifstream t(path + ".tmp");
        VERIFY_MSG(!t.is_open(), "temporary file should not remain");
    }

    printf("[PASS] test_save_replaces_existing_file\n");
}

/** @brief 書き込み先の親パスが不正な場合 (置換の前段で失敗する場合) に旧ファイルが維持されること */
static void test_save_invalid_parent_keeps_old_file() {
    MetricsStore ms;
    ms.set_state(1, "STREAMING");

    std::string path = "test_metrics_keep_tmp.json";
    TempFileGuard guard(path);
    TempFileGuard tmp_guard(path + ".tmp");
    VERIFY_MSG(ms.save_metrics(path), "initial save_metrics should succeed");

    std::string before;
    {
        std::ifstream f(path);
        before.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // 既存ファイルをディレクトリのように扱わせ、置換の前段 (親ディレクトリの作成) で失敗させる
    std::string bad_path = path + "/child.json";
    VERIFY_MSG(!ms.save_metrics(bad_path), "save_metrics to an invalid path should fail");

    std::string after;
    {
        std::ifstream f(path);
        VERIFY_MSG(f.is_open(), "old metrics file should be kept after failed save");
        after.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VERIFY_MSG(before == after, "old metrics file content should be unchanged");

    printf("[PASS] test_save_invalid_parent_keeps_old_file\n");
}

/** @brief リネーム/置換そのものが失敗しても一時ファイルが掃除され、既存の内容が維持されること */
static void test_save_replace_failure_cleans_tmp() {
    MetricsStore ms;
    ms.set_state(1, "STREAMING");

    // 書き込み先をディレクトリにすると、一時ファイルの書き込みは成功し、置換だけが失敗する
    std::string path = "test_metrics_dir_tmp.json";
    std::error_code ec;
    fs::remove_all(path, ec);
    VERIFY_MSG(fs::create_directories(path), "test directory should be created");
    DirGuard guard(path);
    {
        std::ofstream sentinel(fs::path(path) / "sentinel.txt");
        sentinel << "keep";
    }

    VERIFY_MSG(!ms.save_metrics(path), "save_metrics should fail when replace fails");

    VERIFY_MSG(!fs::exists(path + ".tmp"), "temporary file should be removed after replace failure");
    VERIFY_MSG(fs::is_directory(path), "existing destination should be kept after replace failure");
    VERIFY_MSG(fs::exists(fs::path(path) / "sentinel.txt"), "existing destination content should be kept");

    printf("[PASS] test_save_replace_failure_cleans_tmp\n");
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
    test_save_replaces_existing_file();
    test_save_invalid_parent_keeps_old_file();
    test_save_replace_failure_cleans_tmp();
    return 0;
}

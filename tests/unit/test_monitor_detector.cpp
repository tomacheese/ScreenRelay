#include <cstdio>
#include <string>
#include <vector>
#include "monitor/monitor_detector.hpp"
#include "test_utils.hpp"

/**
 * @brief MonitorInfo に stable_id フォールバックロジックを適用するヘルパー
 *
 * enumerate() は Windows API に依存するため直接テストできないが、
 * stable_id のフォールバック規則（空なら device_name で補完する）を
 * 独立して検証する。
 */
static void apply_stable_id_fallback(std::vector<MonitorInfo>& monitors) {
    for (auto& mi : monitors) {
        if (mi.stable_id.empty()) {
            mi.stable_id = mi.device_name;
        }
    }
}

/**
 * @brief MonitorDetector のユニットテストを実行する
 *
 * 実際のモニターアクセスは行わず、ユーティリティ関数のみを検証する。
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_monitor_detector_tests() {
    printf("=== Monitor Detector Tests ===\n");

    // ── extract_number ──────────────────────────────────────────

    {
        // "\\.\DISPLAY2" → 2
        int n = MonitorDetector::extract_number("\\\\.\\DISPLAY2");
        VERIFY_MSG(n == 2, "extract_number should return 2 for DISPLAY2");
        printf("[PASS] test_extract_number_basic: DISPLAY2 → 2\n");
    }

    {
        // "\\.\DISPLAY10" → 10 (2 桁のモニター番号)
        int n = MonitorDetector::extract_number("\\\\.\\DISPLAY10");
        VERIFY_MSG(n == 10, "extract_number should return 10 for DISPLAY10");
        printf("[PASS] test_extract_number_two_digits: DISPLAY10 → 10\n");
    }

    {
        // "Invalid" → -1 (無効なデバイス名)
        int n = MonitorDetector::extract_number("Invalid");
        VERIFY_MSG(n == -1, "extract_number should return -1 for invalid name");
        printf("[PASS] test_extract_number_invalid: Invalid → -1\n");
    }

    {
        // "" → -1 (空文字列)
        int n = MonitorDetector::extract_number("");
        VERIFY_MSG(n == -1, "extract_number should return -1 for empty string");
        printf("[PASS] test_extract_number_empty: (empty) → -1\n");
    }

    {
        // "\\.\DISPLAY1" → 1
        int n = MonitorDetector::extract_number("\\\\.\\DISPLAY1");
        VERIFY_MSG(n == 1, "extract_number should return 1 for DISPLAY1");
        printf("[PASS] test_extract_number_display1: DISPLAY1 → 1\n");
    }

    // ── stable_id フォールバック ─────────────────────────────────

    {
        // CCD が有効な場合: stable_id が CCD パスで設定される
        MonitorInfo mi;
        mi.device_name = "\\\\.\\DISPLAY1";
        mi.stable_id   = "\\\\?\\DISPLAY#DELA0BC#5&1234abcd&0&UID257#{...}";
        std::vector<MonitorInfo> monitors = {mi};
        apply_stable_id_fallback(monitors);
        // stable_id が空でなければフォールバックは適用されない
        VERIFY_MSG(monitors[0].stable_id == "\\\\?\\DISPLAY#DELA0BC#5&1234abcd&0&UID257#{...}",
                   "stable_id should retain CCD path when not empty");
        printf("[PASS] test_stable_id_ccd_path: CCD パスが保持されること\n");
    }

    {
        // CCD 取得失敗時: stable_id が空のとき device_name でフォールバックされる
        MonitorInfo mi;
        mi.device_name = "\\\\.\\DISPLAY2";
        mi.stable_id   = "";  // CCD 失敗を模倣
        std::vector<MonitorInfo> monitors = {mi};
        apply_stable_id_fallback(monitors);
        VERIFY_MSG(monitors[0].stable_id == "\\\\.\\DISPLAY2",
                   "stable_id should fall back to device_name when empty");
        printf("[PASS] test_stable_id_fallback: 空の stable_id は device_name にフォールバックされること\n");
    }

    {
        // 複数モニター混在: CCD 有りと無しが正しく処理される
        MonitorInfo m1, m2;
        m1.device_name = "\\\\.\\DISPLAY1";
        m1.stable_id   = "\\\\?\\DISPLAY#MONITOR1#{...}";
        m2.device_name = "\\\\.\\DISPLAY3";
        m2.stable_id   = "";  // CCD 失敗
        std::vector<MonitorInfo> monitors = {m1, m2};
        apply_stable_id_fallback(monitors);
        VERIFY_MSG(monitors[0].stable_id == "\\\\?\\DISPLAY#MONITOR1#{...}",
                   "first monitor stable_id should be CCD path");
        VERIFY_MSG(monitors[1].stable_id == "\\\\.\\DISPLAY3",
                   "second monitor stable_id should fall back to device_name");
        printf("[PASS] test_stable_id_mixed: CCD 有り/無し混在が正しく処理されること\n");
    }

    return 0;
}

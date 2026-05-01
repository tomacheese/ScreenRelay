#include <cstdio>
#include "monitor/monitor_detector.hpp"
#include "test_utils.hpp"

/**
 * @brief MonitorDetector のユニットテストを実行する
 *
 * 実際のモニターアクセスは行わず、ユーティリティ関数のみを検証する。
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_monitor_detector_tests() {
    printf("=== Monitor Detector Tests ===\n");

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

    return 0;
}

#include <cstdio>
#include <string>
#include <vector>
#include "monitor/monitor_detector.hpp"
#include "test_utils.hpp"

// Mirrors the stable_id fallback rule (an empty stable_id becomes device_name).
// enumerate() depends on Windows APIs and cannot be tested directly, so the rule is
// verified in isolation.
static void apply_stable_id_fallback(std::vector<MonitorInfo>& monitors) {
    for (auto& mi : monitors) {
        if (mi.stable_id.empty()) {
            mi.stable_id = mi.device_name;
        }
    }
}

/**
 * @brief Tests MonitorDetector utilities; no real monitor access.
 * @return 0 on success (a failing VERIFY exits with status 1)
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
        // "\\.\DISPLAY10" → 10 (two-digit monitor number)
        int n = MonitorDetector::extract_number("\\\\.\\DISPLAY10");
        VERIFY_MSG(n == 10, "extract_number should return 10 for DISPLAY10");
        printf("[PASS] test_extract_number_two_digits: DISPLAY10 → 10\n");
    }

    {
        // "Invalid" → -1 (not a device name)
        int n = MonitorDetector::extract_number("Invalid");
        VERIFY_MSG(n == -1, "extract_number should return -1 for invalid name");
        printf("[PASS] test_extract_number_invalid: Invalid → -1\n");
    }

    {
        // "" → -1 (empty string)
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

    // ── stable_id fallback ──────────────────────────────────────

    {
        // CCD available: stable_id holds the CCD path
        MonitorInfo mi;
        mi.device_name = "\\\\.\\DISPLAY1";
        mi.stable_id   = "\\\\?\\DISPLAY#DELA0BC#5&1234abcd&0&UID257#{...}";
        std::vector<MonitorInfo> monitors = {mi};
        apply_stable_id_fallback(monitors);
        VERIFY_MSG(monitors[0].stable_id == "\\\\?\\DISPLAY#DELA0BC#5&1234abcd&0&UID257#{...}",
                   "stable_id should retain CCD path when not empty");
        printf("[PASS] test_stable_id_ccd_path: CCD path is preserved\n");
    }

    {
        // CCD lookup failed: an empty stable_id falls back to device_name
        MonitorInfo mi;
        mi.device_name = "\\\\.\\DISPLAY2";
        mi.stable_id   = "";  // simulates a CCD failure
        std::vector<MonitorInfo> monitors = {mi};
        apply_stable_id_fallback(monitors);
        VERIFY_MSG(monitors[0].stable_id == "\\\\.\\DISPLAY2",
                   "stable_id should fall back to device_name when empty");
        printf("[PASS] test_stable_id_fallback: empty stable_id falls back to device_name\n");
    }

    {
        // Mixed monitors: one with a CCD path, one without
        MonitorInfo m1, m2;
        m1.device_name = "\\\\.\\DISPLAY1";
        m1.stable_id   = "\\\\?\\DISPLAY#MONITOR1#{...}";
        m2.device_name = "\\\\.\\DISPLAY3";
        m2.stable_id   = "";  // CCD failure
        std::vector<MonitorInfo> monitors = {m1, m2};
        apply_stable_id_fallback(monitors);
        VERIFY_MSG(monitors[0].stable_id == "\\\\?\\DISPLAY#MONITOR1#{...}",
                   "first monitor stable_id should be CCD path");
        VERIFY_MSG(monitors[1].stable_id == "\\\\.\\DISPLAY3",
                   "second monitor stable_id should fall back to device_name");
        printf("[PASS] test_stable_id_mixed: mixed CCD / no-CCD monitors are handled correctly\n");
    }

    return 0;
}

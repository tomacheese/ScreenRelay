#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

/** @brief Information about a detected monitor (as enumerated by EnumDisplayMonitors). */
struct MonitorInfo {
    HMONITOR handle         = nullptr;
    int      number         = 0;          ///< The n in \\.\DISPLAYn
    int      logical_width  = 0;          ///< Logical width (after DPI scaling)
    int      logical_height = 0;          ///< Logical height (after DPI scaling)
    bool     is_primary     = false;      ///< true for the primary monitor
    std::string device_name;              ///< Device name (e.g. "\\.\DISPLAY2")
    std::string stable_id;               ///< ID stable per physical monitor: the CCD API
                                          ///<   monitorDevicePath (e.g.
                                          ///<   "\\\\?\\DISPLAY#DELA0BC#...#{...}").
                                          ///<   Falls back to device_name if CCD lookup fails.
                                          ///<   Used as the pipeline management key.
};

/**
 * @brief Enumerates connected monitors, excluding mirroring drivers.
 *
 * Uses EnumDisplayMonitors / EnumDisplayDevices.
 */
class MonitorDetector {
public:
    /**
     * @brief Enumerates currently connected monitors (excluding mirrors).
     * @return The detected monitors
     */
    static std::vector<MonitorInfo> enumerate();

    /**
     * @brief Extracts the monitor number from a device name.
     * @param device_name  Device name (e.g. "\\.\DISPLAY2")
     * @return The number (2 in the example), or -1 on failure
     */
    static int extract_number(const std::string& device_name);

    /**
     * @brief Returns whether the device is a mirroring driver.
     * @param device_name  Device name
     * @return true for a mirroring driver
     */
    static bool is_mirror_driver(const std::string& device_name);
};

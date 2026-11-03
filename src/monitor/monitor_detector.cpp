#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "monitor/monitor_detector.hpp"
#include <cctype>
#include <map>
#include <string>
#include <vector>

/**
 * @brief EnumDisplayMonitors callback: skips mirroring drivers and appends a MonitorInfo.
 *
 * @param lparam  Pointer to the std::vector<MonitorInfo> result
 * @return Always TRUE so enumeration continues
 */
static BOOL CALLBACK monitor_enum_callback(HMONITOR hmon, HDC /*hdc*/, LPRECT /*rect*/, LPARAM lparam) {
    auto* result = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(hmon, &info)) {
        return TRUE;
    }

    // WCHAR device name -> UTF-8 std::string.
    std::string device_name;
    {
        int len = WideCharToMultiByte(
            CP_UTF8, 0,
            info.szDevice, -1,
            nullptr, 0,
            nullptr, nullptr
        );
        if (len > 0) {
            device_name.resize(static_cast<size_t>(len - 1));  // exclude the null terminator
            WideCharToMultiByte(
                CP_UTF8, 0,
                info.szDevice, -1,
                device_name.data(), len - 1,  // length without the null terminator
                nullptr, nullptr
            );
        }
    }

    if (MonitorDetector::is_mirror_driver(device_name)) {
        return TRUE;
    }

    int number = MonitorDetector::extract_number(device_name);
    if (number < 0) {
        return TRUE;
    }

    MonitorInfo mi;
    mi.handle       = hmon;
    mi.number       = number;
    mi.device_name  = device_name;
    mi.is_primary   = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // Logical resolution comes from rcMonitor.
    mi.logical_width  = info.rcMonitor.right  - info.rcMonitor.left;
    mi.logical_height = info.rcMonitor.bottom - info.rcMonitor.top;

    result->push_back(mi);
    return TRUE;
}

/**
 * @brief Builds a GDI device name -> stable physical-monitor ID map via the CCD API.
 *
 * Enumerates active display paths with `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` and
 * maps each path's source name (`\\.\DISPLAYn`) to its target's `monitorDevicePath`.
 * The path embeds the monitor's EDID/device instance ID, so it stays the same
 * for a given physical panel across power-off, disconnect and reboot.
 *
 * Returns an empty map on failure; callers fall back to device_name.
 *
 * @return Map of device name ("\\.\DISPLAYn") -> monitorDevicePath
 */
static std::map<std::string, std::string> build_ccd_table() {
    std::map<std::string, std::string> table;

    UINT32 num_paths = 0, num_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes)
            != ERROR_SUCCESS) {
        return table;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(num_paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(num_modes);

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                            &num_paths, paths.data(),
                            &num_modes, modes.data(),
                            nullptr) != ERROR_SUCCESS) {
        return table;
    }

    for (UINT32 i = 0; i < num_paths; ++i) {
        // Source side: GDI device name.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src_name{};
        src_name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src_name.header.size      = sizeof(src_name);
        src_name.header.adapterId = paths[i].sourceInfo.adapterId;
        src_name.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src_name.header) != ERROR_SUCCESS) continue;

        // Target side: physical monitor device path.
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt_name{};
        tgt_name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt_name.header.size      = sizeof(tgt_name);
        tgt_name.header.adapterId = paths[i].targetInfo.adapterId;
        tgt_name.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt_name.header) != ERROR_SUCCESS) continue;

        // WCHAR -> UTF-8: source GDI name
        std::string gdi_name;
        {
            int len = WideCharToMultiByte(
                CP_UTF8, 0,
                src_name.viewGdiDeviceName, -1,
                nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                gdi_name.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(
                    CP_UTF8, 0,
                    src_name.viewGdiDeviceName, -1,
                    gdi_name.data(), len - 1, nullptr, nullptr);
            }
        }

        // WCHAR -> UTF-8: target monitorDevicePath
        std::string monitor_path;
        {
            int len = WideCharToMultiByte(
                CP_UTF8, 0,
                tgt_name.monitorDevicePath, -1,
                nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                monitor_path.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(
                    CP_UTF8, 0,
                    tgt_name.monitorDevicePath, -1,
                    monitor_path.data(), len - 1, nullptr, nullptr);
            }
        }

        if (!gdi_name.empty() && !monitor_path.empty()) {
            table[gdi_name] = monitor_path;
        }
    }

    return table;
}

/**
 * @brief Enumerates connected monitors (excluding mirrors).
 *
 * Fills stable_id from the CCD monitorDevicePath, falling back to device_name
 * when the CCD lookup fails.
 */
std::vector<MonitorInfo> MonitorDetector::enumerate() {
    std::vector<MonitorInfo> result;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_callback, reinterpret_cast<LPARAM>(&result));

    auto ccd_table = build_ccd_table();
    for (auto& mi : result) {
        auto it = ccd_table.find(mi.device_name);
        if (it != ccd_table.end() && !it->second.empty()) {
            mi.stable_id = it->second;
        } else {
            // CCD lookup failed: fall back to device_name.
            mi.stable_id = mi.device_name;
        }
    }

    return result;
}

/**
 * @brief Extracts the trailing digits of the device name as the monitor number.
 *
 * "\\.\DISPLAY2" -> 2, "\\.\DISPLAY10" -> 10.
 *
 * @return The number, or -1 on failure
 */
int MonitorDetector::extract_number(const std::string& device_name) {
    if (device_name.empty()) {
        return -1;
    }

    size_t end = device_name.size();
    size_t pos = end;

    while (pos > 0 && std::isdigit(static_cast<unsigned char>(device_name[pos - 1]))) {
        --pos;
    }

    if (pos == end) {
        return -1;
    }

    try {
        int num = std::stoi(device_name.substr(pos, end - pos));
        return num;
    } catch (...) {
        return -1;
    }
}

/** Checks DISPLAY_DEVICE_MIRRORING_DRIVER in the StateFlags from EnumDisplayDevices. */
bool MonitorDetector::is_mirror_driver(const std::string& device_name) {
    // UTF-8 -> wchar_t
    std::wstring wname;
    {
        int len = MultiByteToWideChar(
            CP_UTF8, 0,
            device_name.c_str(), -1,
            nullptr, 0
        );
        if (len > 0) {
            wname.resize(static_cast<size_t>(len - 1));  // exclude the null terminator
            MultiByteToWideChar(
                CP_UTF8, 0,
                device_name.c_str(), -1,
                wname.data(), len - 1  // length without the null terminator
            );
        }
    }

    if (wname.empty()) {
        return false;
    }

    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(DISPLAY_DEVICEW);

    if (EnumDisplayDevicesW(wname.c_str(), 0, &dd, 0)) {
        return (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
    }

    return false;
}

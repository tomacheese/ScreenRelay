#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "monitor/monitor_detector.hpp"
#include <cctype>
#include <string>
#include <vector>

/**
 * @brief EnumDisplayMonitors のコールバック関数
 *
 * 列挙されたモニターごとに呼び出される。ミラーリングドライバーを除外した後、
 * MonitorInfo を構築して結果ベクターに追加する。
 *
 * @param hmon    モニターハンドル
 * @param hdc     デバイスコンテキスト (未使用)
 * @param rect    モニター矩形 (未使用。MONITORINFOEX から取得)
 * @param lparam  MonitorInfo ベクターへのポインタ
 * @return 列挙を継続するため常に TRUE を返す
 */
static BOOL CALLBACK monitor_enum_callback(HMONITOR hmon, HDC /*hdc*/, LPRECT /*rect*/, LPARAM lparam) {
    auto* result = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);

    // MONITORINFOEX でデバイス名を取得する
    MONITORINFOEXW info{};
    info.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(hmon, &info)) {
        return TRUE;
    }

    // WCHAR デバイス名を std::string (UTF-8 / マルチバイト) に変換する
    std::string device_name;
    {
        // 必要なバッファサイズを計算する
        int len = WideCharToMultiByte(
            CP_UTF8, 0,
            info.szDevice, -1,
            nullptr, 0,
            nullptr, nullptr
        );
        if (len > 0) {
            device_name.resize(static_cast<size_t>(len - 1));  // null 終端分を除く
            WideCharToMultiByte(
                CP_UTF8, 0,
                info.szDevice, -1,
                device_name.data(), len,
                nullptr, nullptr
            );
        }
    }

    // ミラーリングドライバーは除外する
    if (MonitorDetector::is_mirror_driver(device_name)) {
        return TRUE;
    }

    // デバイス名からモニター番号を抽出する
    int number = MonitorDetector::extract_number(device_name);
    if (number < 0) {
        return TRUE;
    }

    // MonitorInfo を組み立てる
    MonitorInfo mi;
    mi.handle       = hmon;
    mi.number       = number;
    mi.device_name  = device_name;
    mi.is_primary   = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // 論理解像度は MONITORINFOEX の rcMonitor から計算する
    mi.logical_width  = info.rcMonitor.right  - info.rcMonitor.left;
    mi.logical_height = info.rcMonitor.bottom - info.rcMonitor.top;

    result->push_back(mi);
    return TRUE;
}

// ---------------------------------------------------------------------------
// MonitorDetector の実装
// ---------------------------------------------------------------------------

/**
 * @brief 現在接続されている (ミラーを除く) モニターを列挙する
 * @return 検出されたモニターの一覧
 */
std::vector<MonitorInfo> MonitorDetector::enumerate() {
    std::vector<MonitorInfo> result;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_callback, reinterpret_cast<LPARAM>(&result));
    return result;
}

/**
 * @brief デバイス名からモニター番号を抽出する
 *
 * "\\.\DISPLAY2" → 2、"\\.\DISPLAY10" → 10 のように、
 * デバイス名末尾の連続する数字部分を整数として返す。
 *
 * @param device_name  デバイス名
 * @return モニター番号。抽出失敗時は -1
 */
int MonitorDetector::extract_number(const std::string& device_name) {
    if (device_name.empty()) {
        return -1;
    }

    // 末尾から数字を探す
    size_t end = device_name.size();
    size_t pos = end;

    while (pos > 0 && std::isdigit(static_cast<unsigned char>(device_name[pos - 1]))) {
        --pos;
    }

    // 数字が見つからない場合は失敗
    if (pos == end) {
        return -1;
    }

    // 数字部分を整数に変換する
    try {
        int num = std::stoi(device_name.substr(pos, end - pos));
        return num;
    } catch (...) {
        return -1;
    }
}

/**
 * @brief ミラーリングドライバかどうかを判定する
 *
 * EnumDisplayDevices を使用してデバイスの StateFlags を確認し、
 * DISPLAY_DEVICE_MIRRORING_DRIVER フラグが立っている場合はミラーリングドライバーと判断する。
 *
 * @param device_name  デバイス名
 * @return ミラーリングドライバなら true
 */
bool MonitorDetector::is_mirror_driver(const std::string& device_name) {
    // デバイス名を wchar_t に変換する
    std::wstring wname;
    {
        int len = MultiByteToWideChar(
            CP_UTF8, 0,
            device_name.c_str(), -1,
            nullptr, 0
        );
        if (len > 0) {
            wname.resize(static_cast<size_t>(len - 1));  // null 終端分を除く
            MultiByteToWideChar(
                CP_UTF8, 0,
                device_name.c_str(), -1,
                wname.data(), len
            );
        }
    }

    if (wname.empty()) {
        return false;
    }

    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(DISPLAY_DEVICEW);

    // iDevNum=0 でデバイス名に対応する情報を取得する
    if (EnumDisplayDevicesW(wname.c_str(), 0, &dd, 0)) {
        return (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
    }

    return false;
}

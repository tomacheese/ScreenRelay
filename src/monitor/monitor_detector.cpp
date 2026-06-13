#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "monitor/monitor_detector.hpp"
#include <cctype>
#include <map>
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
                device_name.data(), len - 1,  // null 終端を除いた長さを渡す
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
// CCD ヘルパー
// ---------------------------------------------------------------------------

/**
 * @brief CCD API を使用して GDI デバイス名 → 物理モニター安定 ID のマップを構築する
 *
 * `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` でアクティブなディスプレイパスを列挙し、
 * 各パスのソース名（= `\\.\DISPLAYn`）とターゲット名（物理モニターの `monitorDevicePath`）を
 * 対応付けたマップを返す。
 *
 * `monitorDevicePath` はモニターの EDID/デバイスインスタンス ID を含み、
 * 電源断・切断・再起動を経ても同じ物理パネルに対して変化しない安定 ID として機能する。
 *
 * 失敗（API 非対応環境・エラー）時は空のマップを返す。呼び出し元が device_name を
 * フォールバックとして使用することで堅牢性を保つ。
 *
 * @return デバイス名（"\\.\DISPLAYn"）→ monitorDevicePath のマップ
 */
static std::map<std::string, std::string> build_ccd_table() {
    std::map<std::string, std::string> table;

    // アクティブパス数とモード数を取得する
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
        // ソース側: GDI デバイス名（"\\.\DISPLAYn"）を取得する
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src_name{};
        src_name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src_name.header.size      = sizeof(src_name);
        src_name.header.adapterId = paths[i].sourceInfo.adapterId;
        src_name.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src_name.header) != ERROR_SUCCESS) continue;

        // ターゲット側: 物理モニターのデバイスパスを取得する
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt_name{};
        tgt_name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt_name.header.size      = sizeof(tgt_name);
        tgt_name.header.adapterId = paths[i].targetInfo.adapterId;
        tgt_name.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt_name.header) != ERROR_SUCCESS) continue;

        // WCHAR → UTF-8 変換: ソース GDI 名
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

        // WCHAR → UTF-8 変換: ターゲット monitorDevicePath
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

// ---------------------------------------------------------------------------
// MonitorDetector の実装
// ---------------------------------------------------------------------------

/**
 * @brief 現在接続されている (ミラーを除く) モニターを列挙する
 *
 * EnumDisplayMonitors でモニターを列挙した後、CCD API（QueryDisplayConfig）で
 * 取得した物理モニター安定 ID（monitorDevicePath）を各 MonitorInfo の
 * stable_id フィールドに格納する。CCD 取得失敗時は device_name をフォールバックとして使用する。
 *
 * @return 検出されたモニターの一覧
 */
std::vector<MonitorInfo> MonitorDetector::enumerate() {
    std::vector<MonitorInfo> result;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_callback, reinterpret_cast<LPARAM>(&result));

    // CCD テーブルを構築して stable_id を補完する
    auto ccd_table = build_ccd_table();
    for (auto& mi : result) {
        auto it = ccd_table.find(mi.device_name);
        if (it != ccd_table.end() && !it->second.empty()) {
            mi.stable_id = it->second;
        } else {
            // CCD 取得失敗時は device_name をフォールバックとして使用する
            mi.stable_id = mi.device_name;
        }
    }

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
                wname.data(), len - 1  // null 終端を除いた長さを渡す
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

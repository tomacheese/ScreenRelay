#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

/**
 * @brief 検出済みモニターの情報
 *
 * EnumDisplayMonitors で列挙したモニターの識別子・解像度・プライマリ判定などを保持する。
 */
struct MonitorInfo {
    HMONITOR handle         = nullptr;
    int      number         = 0;          ///< \\.\DISPLAYn の n
    int      logical_width  = 0;          ///< 論理解像度の幅 (DPI スケーリング後)
    int      logical_height = 0;          ///< 論理解像度の高さ (DPI スケーリング後)
    bool     is_primary     = false;      ///< プライマリモニターなら true
    std::string device_name;              ///< デバイス名 (例: "\\.\DISPLAY2")
};

/**
 * @brief モニターの列挙と変更検知を行うクラス
 *
 * Windows の EnumDisplayMonitors / EnumDisplayDevices を使用して、
 * 接続中のモニター一覧を取得し、ミラーリングドライバーを除外する。
 */
class MonitorDetector {
public:
    /**
     * @brief 現在接続されている (ミラーを除く) モニターを列挙する
     * @return 検出されたモニターの一覧
     */
    static std::vector<MonitorInfo> enumerate();

    /**
     * @brief デバイス名からモニター番号を抽出する
     * @param device_name  デバイス名 (例: "\\.\DISPLAY2")
     * @return モニター番号 (2)、抽出失敗時は -1
     */
    static int extract_number(const std::string& device_name);

    /**
     * @brief ミラーリングドライバかどうかを判定する
     * @param device_name  デバイス名
     * @return ミラーリングドライバなら true
     */
    static bool is_mirror_driver(const std::string& device_name);
};

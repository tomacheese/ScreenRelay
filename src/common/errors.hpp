#pragma once
#include <string>

/**
 * @brief アプリケーション全体で使用するエラーコード定数
 *
 * エラーコードは英語の大文字スネークケースで定義する。
 * ログやメトリクスの "code" フィールドに使用される。
 */
namespace errors {
    /** キャプチャの初期化失敗 */
    constexpr const char* CAPTURE_INIT_FAILED   = "CAPTURE_INIT_FAILED";
    /** エンコーダーの初期化失敗 */
    constexpr const char* ENCODER_INIT_FAILED   = "ENCODER_INIT_FAILED";
    /** エンコード処理の失敗 */
    constexpr const char* ENCODER_ENCODE_FAILED = "ENCODER_ENCODE_FAILED";
    /** RTSP 接続の失敗 */
    constexpr const char* RTSP_CONNECT_FAILED   = "RTSP_CONNECT_FAILED";
    /** RTSP 送信の失敗 */
    constexpr const char* RTSP_SEND_FAILED      = "RTSP_SEND_FAILED";
    /** RTSP タイムアウト */
    constexpr const char* RTSP_TIMEOUT          = "RTSP_TIMEOUT";
    /** モニター列挙の失敗 */
    constexpr const char* MONITOR_ENUM_FAILED   = "MONITOR_ENUM_FAILED";
    /** GPU クラッシュ */
    constexpr const char* GPU_CRASH             = "GPU_CRASH";
} // namespace errors

#pragma once
#include <string>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

/**
 * @brief JSON Lines 形式のログシンク
 *
 * アプリケーションイベントを JSON Lines 形式でファイルおよび
 * コンソールに出力する。モニター番号をフィールドとして含む。
 */
class LogSink {
public:
    /** @brief コンストラクタ */
    LogSink();

    /** @brief デストラクタ。バッファをフラッシュする。 */
    ~LogSink();

    /**
     * @brief ロガーを初期化する
     * @param log_dir       ログ出力ディレクトリ
     * @param instance_name アプリインスタンス名 (ファイル名に使用)
     * @param log_level     ログレベル文字列 (trace/debug/info/warn/error/critical)
     * @param error         エラーメッセージ出力先
     * @return 初期化成功時 true、失敗時 false
     */
    bool init(const std::string& log_dir,
              const std::string& instance_name,
              const std::string& log_level,
              std::string& error);

    /**
     * @brief 任意のイベントをログに記録する
     * @param level      ログレベル
     * @param event_name イベント名
     * @param fields     追加フィールドのマップ
     */
    void log_event(spdlog::level::level_enum level,
                   const std::string& event_name,
                   const std::map<std::string, std::string>& fields = {});

    /**
     * @brief ステート変化をログに記録する
     * @param monitor_number モニター番号
     * @param from           変化前のステート名
     * @param to             変化後のステート名
     */
    void log_state_changed(int monitor_number,
                           const std::string& from, const std::string& to);

    /**
     * @brief エラーをログに記録する
     * @param code    エラーコード (errors:: 定数を使用)
     * @param message エラーメッセージ
     * @param extra   追加フィールドのマップ
     */
    void log_error(const std::string& code,
                   const std::string& message,
                   const std::map<std::string, std::string>& extra = {});

    /**
     * @brief 配信開始イベントをログに記録する
     * @param monitor_number モニター番号
     * @param url            RTSP 配信 URL
     * @param width          映像幅 (ピクセル)
     * @param height         映像高さ (ピクセル)
     * @param fps            フレームレート
     */
    void log_publish_started(int monitor_number, const std::string& url,
                             int width, int height, int fps);

    /**
     * @brief 配信停止イベントをログに記録する
     * @param monitor_number モニター番号
     * @param reason         停止理由
     */
    void log_publish_stopped(int monitor_number, const std::string& reason);

    /**
     * @brief 再接続イベントをログに記録する
     * @param monitor_number モニター番号
     * @param attempt        試行回数
     * @param delay_ms       次回接続までの待機時間 (ms)
     */
    void log_reconnect(int monitor_number, int attempt, int delay_ms);

    /**
     * @brief エンコーダー初期化イベントをログに記録する
     * @param monitor_number モニター番号
     * @param codec          使用コーデック名
     * @param width          映像幅 (ピクセル)
     * @param height         映像高さ (ピクセル)
     * @param fps            フレームレート
     * @param bitrate_kbps   ビットレート (kbps)
     */
    void log_encoder_initialized(int monitor_number, const std::string& codec,
                                 int width, int height, int fps, int bitrate_kbps);

    /**
     * @brief モニター追加イベントをログに記録する
     * @param monitor_number モニター番号
     * @param device_name    デバイス名
     * @param width          解像度幅 (ピクセル)
     * @param height         解像度高さ (ピクセル)
     * @param is_primary     プライマリモニターフラグ
     */
    void log_monitor_added(int monitor_number, const std::string& device_name,
                           int width, int height, bool is_primary);

    /**
     * @brief モニター削除イベントをログに記録する
     * @param monitor_number 削除されたモニターの番号
     */
    void log_monitor_removed(int monitor_number);

    /** @brief バッファをフラッシュする */
    void flush();

private:
    /**
     * @brief JSON Lines 形式の文字列を構築する
     * @param event_name イベント名
     * @param fields     フィールドのマップ
     * @return JSON 文字列
     */
    std::string build_json(const std::string& event_name,
                           const std::map<std::string, std::string>& fields);

    std::shared_ptr<spdlog::logger> logger_;  ///< spdlog ロガーインスタンス
};

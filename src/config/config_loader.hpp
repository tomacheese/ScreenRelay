#pragma once
#include "common/types.hpp"
#include <string>

/**
 * @brief アプリケーション全体の設定を保持する構造体
 *
 * JSON 設定ファイルから読み込まれた各セクションの値を格納する。
 * デフォルト値はフィールド初期化子で定義される。
 */
struct AppConfig {
    /** @brief アプリケーション共通設定 */
    struct App {
        std::string instance_name = "screen-relay";          ///< インスタンス名 (ログファイル名に使用)
        std::string log_dir       = "./logs";                 ///< ログ出力ディレクトリ
        std::string log_level     = "info";                   ///< ログレベル (trace/debug/info/warn/error/critical)
        std::string metrics_path  = "./state/metrics.json";  ///< メトリクス出力ファイルパス
        std::string health_path   = "./state/health.json";   ///< ヘルスチェック出力ファイルパス
    } app;

    /** @brief キャプチャ設定 */
    struct Capture {
        std::string backend       = "dxgi";  ///< キャプチャバックエンド (現時点は "dxgi" のみ)
        int frame_timeout_ms      = 100;     ///< フレーム取得タイムアウト (ms)
    } capture;

    EncoderConfig encoder;  ///< エンコーダー設定
    RtspConfig    rtsp;     ///< RTSP 配信設定
    AudioConfig   audio;    ///< 音声配信設定

    /** @brief ランタイム動作設定 */
    struct Runtime {
        int monitor_check_interval_ms = 1000;  ///< モニター接続変化チェック間隔 (ms)
        int shutdown_grace_ms         = 3000;  ///< グレースフルシャットダウン待機時間 (ms)
        int emit_metrics_interval_ms  = 1000;  ///< メトリクス書き込み間隔 (ms)
    } runtime;
};

/**
 * @brief 設定ファイルのロードを担当するクラス
 *
 * JSON 形式の設定ファイルを読み込み、AppConfig に変換する。
 * バリデーションエラーは error パラメータを通じて通知される。
 */
class ConfigLoader {
public:
    /**
     * @brief JSON 設定ファイルを読み込む
     * @param path    設定ファイルのパス
     * @param out     読み込み結果の出力先
     * @param error   エラーメッセージの出力先
     * @return 成功した場合 true、失敗した場合 false
     */
    static bool load(const std::string& path, AppConfig& out, std::string& error);
};

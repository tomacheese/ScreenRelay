#pragma once
#include <string>
#include <mutex>
#include <map>
#include <cstdint>

/**
 * @brief スレッドセーフなメトリクスストア
 *
 * モニター番号ごとにパイプラインの統計情報を管理する。
 * mutex + map<int, MonitorStats> によるシンプルな実装を採用している。
 * ファイルへの書き込みはアトミックに行われる。
 */
class MetricsStore {
public:
    /** @brief コンストラクタ */
    MetricsStore();

    /**
     * @brief モニターのパイプラインステートを更新する
     * @param monitor_number モニター番号
     * @param state          ステート名 (state_name() の戻り値)
     */
    void set_state(int monitor_number, const std::string& state);

    /**
     * @brief モニターの解像度を更新する
     * @param monitor_number モニター番号
     * @param w              幅 (ピクセル)
     * @param h              高さ (ピクセル)
     */
    void set_resolution(int monitor_number, int w, int h);

    /**
     * @brief 現在の FPS を更新する
     * @param monitor_number モニター番号
     * @param fps            測定 FPS
     */
    void set_current_fps(int monitor_number, double fps);

    /**
     * @brief 現在のビットレートを更新する
     * @param monitor_number モニター番号
     * @param kbps           ビットレート (kbps)
     */
    void set_bitrate_kbps(int monitor_number, double kbps);

    /**
     * @brief 使用中のコーデックを更新する
     * @param monitor_number モニター番号
     * @param codec          コーデック名
     */
    void set_encoder_codec(int monitor_number, const std::string& codec);

    /**
     * @brief 受信フレーム数をインクリメントする
     * @param monitor_number モニター番号
     */
    void increment_frames_received(int monitor_number);

    /**
     * @brief エンコード済みフレーム数をインクリメントする
     * @param monitor_number モニター番号
     */
    void increment_frames_encoded(int monitor_number);

    /**
     * @brief ドロップフレーム数をインクリメントする
     * @param monitor_number モニター番号
     */
    void increment_frames_dropped(int monitor_number);

    /**
     * @brief RTSP エラー数をインクリメントする
     * @param monitor_number モニター番号
     */
    void increment_rtsp_errors(int monitor_number);

    /**
     * @brief 再接続試行数をインクリメントする
     * @param monitor_number モニター番号
     */
    void increment_reconnect_attempts(int monitor_number);

    /**
     * @brief セッション開始時刻を記録する
     *
     * アプリケーション起動時に一度だけ呼び出す。
     * uptime_ms の計算基準となる。
     */
    void mark_session_start();

    /**
     * @brief メトリクスを JSON ファイルに保存する
     * @param path 保存先ファイルパス
     * @return 成功時 true
     */
    bool save_metrics(const std::string& path) const;

    /**
     * @brief ヘルスチェック結果を JSON ファイルに保存する
     * @param path 保存先ファイルパス
     * @return 成功時 true
     */
    bool save_health(const std::string& path) const;

private:
    /**
     * @brief モニターごとの統計データ
     *
     * ステート・解像度・FPS・ビットレート・コーデックおよび
     * 各種カウンターをひとつの構造体にまとめる。
     */
    struct MonitorStats {
        std::string state;           ///< パイプラインステート名
        int width          = 0;      ///< 映像幅 (ピクセル)
        int height         = 0;      ///< 映像高さ (ピクセル)
        double fps         = 0.0;    ///< 現在の FPS
        double bitrate_kbps = 0.0;  ///< 現在のビットレート (kbps)
        std::string codec;           ///< 使用中のコーデック名
        uint64_t frames_received    = 0;  ///< 受信フレーム数
        uint64_t frames_encoded     = 0;  ///< エンコード済みフレーム数
        uint64_t frames_dropped     = 0;  ///< ドロップフレーム数
        uint64_t rtsp_errors        = 0;  ///< RTSP エラー数
        uint64_t reconnect_attempts = 0;  ///< 再接続試行数
    };

    /**
     * @brief 指定モニターの統計データを取得する (なければ作成)
     * @param monitor_number モニター番号
     * @return MonitorStats への参照 (mutex 保護下で使用すること)
     */
    MonitorStats& get_stats(int monitor_number);

    /**
     * @brief メトリクス JSON 文字列を構築する
     * @return フォーマット済み JSON 文字列
     */
    std::string build_metrics_json() const;

    /**
     * @brief ヘルス JSON 文字列を構築する
     * @return フォーマット済み JSON 文字列
     */
    std::string build_health_json() const;

    mutable std::mutex mutex_;          ///< 統計データ保護用 mutex
    std::map<int, MonitorStats> stats_; ///< モニター番号→統計データのマップ
    int64_t session_start_ms_ = 0;      ///< セッション開始時刻 (UNIX ms)
};

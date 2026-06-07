#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "config/config_loader.hpp"
#include "common/time_utils.hpp"
#include "app/state_machine.hpp"
#include "logging/log_sink.hpp"
#include "metrics/metrics_store.hpp"
#include "monitor/monitor_detector.hpp"
#include "capture/capture_backend.hpp"
#include "capture/dxgi_capture.hpp"
#include "capture/frame_pump.hpp"
#include "encoder/encoder_controller.hpp"
#include "rtsp/rtsp_publisher_client.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ─── ScreenPipeline ────────────────────────────────────────

/**
 * @brief モニター 1 台分のキャプチャ→エンコード→RTSP 配信パイプライン
 *
 * 各モニターに対してひとつのインスタンスが生成される。
 * パイプラインスレッドが内部ステートマシンに従って各フェーズを実行する。
 * プライマリモニターの場合は 2 本の RTSP ストリーム (screen{n} と screen0) を配信する。
 */
class ScreenPipeline {
public:
    /**
     * @brief コンストラクタ
     * @param info     対象モニターの情報
     * @param config   アプリケーション設定
     * @param log      ログシンク
     * @param metrics  メトリクスストア
     */
    ScreenPipeline(const MonitorInfo& info, const AppConfig& config,
                   std::shared_ptr<LogSink> log,
                   std::shared_ptr<MetricsStore> metrics);

    /**
     * @brief デストラクタ
     *
     * stop() を呼び出してパイプラインスレッドを終了する。
     */
    ~ScreenPipeline();

    /** @brief パイプラインを開始する */
    void start();

    /** @brief パイプラインを停止する */
    void stop();

    /**
     * @brief 解像度変更を通知する
     * @param new_width  新しい論理幅 (px)
     * @param new_height 新しい論理高さ (px)
     */
    void notify_resized(int new_width, int new_height);

    /**
     * @brief 現在のステートを返す
     * @return 現在の PipelineState
     */
    PipelineState state() const { return state_machine_.current_state(); }

    /**
     * @brief モニター番号を返す
     * @return モニター番号
     */
    int monitor_number() const { return monitor_info_.number; }

    /**
     * @brief FATAL ステートかどうかを返す
     * @return FATAL ステートなら true
     */
    bool is_fatal() const { return state() == PipelineState::FATAL; }

private:
    /**
     * @brief パイプラインスレッドのメイン関数
     *
     * ステートマシンに従って各フェーズを繰り返し実行する。
     */
    void pipeline_thread_func();

    /**
     * @brief DXGI キャプチャと FramePump を初期化する (INIT → CAPTURING)
     * @return 成功した場合 true
     */
    bool do_init_and_capture();

    /**
     * @brief エンコーダーと RTSP クライアントを初期化して接続する (CAPTURING → STREAMING)
     * @return 成功した場合 true。失敗時は CAPTURING に留まる (リトライ可能)
     */
    bool do_connect();

    /**
     * @brief エンコード・送信ループを実行する (STREAMING 中)
     *
     * stop_requested_ または RTSP エラー・解像度変更が発生するまでループする。
     */
    void do_streaming_loop();

    /**
     * @brief RTSP 再接続をバックオフ付きで実行する (RECONNECTING → STREAMING)
     */
    void do_reconnect();

    /**
     * @brief 解像度変更に対応して再初期化する (RECONFIGURING → CAPTURING)
     */
    void do_reconfigure();

    /**
     * @brief すべてのパイプラインリソースを解放する
     */
    void teardown_all();

    /**
     * @brief RTSP URL を生成する
     *
     * 通常は 1 本、プライマリモニターの場合は screen{n} と screen0 の 2 本を返す。
     *
     * @return RTSP URL のリスト
     */
    std::vector<std::string> make_rtsp_urls() const;

    /**
     * @brief DXGI キャプチャバックエンドをリトライ付きで初期化する
     *
     * 解像度変更直後や GPU を共有する別モニターの RECONFIGURING と競合した場合、
     * DuplicateOutput が E_ACCESSDENIED を返すことがある。200ms 間隔で最大 15 回
     * リトライし、stop_requested_ が立てば途中で打ち切る。
     *
     * do_init_and_capture() と do_reconfigure() で重複していた
     * リトライループをここに集約する。
     *
     * @param backend 初期化対象のバックエンド (null 不可)
     * @param width   論理幅 (px)
     * @param height  論理高さ (px)
     * @return 初期化に成功した場合 true
     */
    bool init_capture_with_retry(ICaptureBackend* backend, int width, int height);

    MonitorInfo   monitor_info_;                ///< 対象モニター情報
    AppConfig     config_;                      ///< アプリケーション設定
    std::shared_ptr<LogSink>      log_;         ///< ログシンク
    std::shared_ptr<MetricsStore> metrics_;     ///< メトリクスストア

    std::unique_ptr<ICaptureBackend>              capture_backend_;  ///< キャプチャバックエンド
    std::unique_ptr<FramePump>                    frame_pump_;       ///< フレームポンプ
    std::unique_ptr<EncoderController>            encoder_;          ///< エンコーダー
    std::vector<std::unique_ptr<RtspPublisherClient>> rtsp_clients_; ///< RTSP クライアントリスト

    StateMachine  state_machine_;              ///< パイプラインステートマシン
    std::thread   pipeline_thread_;            ///< パイプラインスレッド

    std::atomic<bool> stop_requested_{false};  ///< 停止リクエストフラグ
    std::atomic<bool> resize_pending_{false};  ///< 解像度変更ペンディングフラグ

    struct PendingResize {
        int width  = 0;
        int height = 0;
    };
    mutable std::mutex resize_mutex_;   ///< pending_resize_ 保護用ミューテックス
    PendingResize      pending_resize_; ///< 次の解像度 (resize_mutex_ 保護下で読み書き)

    BackoffState  rtsp_backoff_;               ///< RTSP 再接続バックオフ状態
    int           current_width_  = 0;         ///< 現在のフレーム幅
    int           current_height_ = 0;         ///< 現在のフレーム高さ

    /// @brief do_init_and_capture() で取得した最初のフレーム。
    ///        do_streaming_loop() の初期フリーズフレームとして使用し、
    ///        DXGI が次のフレームを返すまでの間（アイドルモニターでは
    ///        24 秒以上かかることがある）もストリームに映像が届くようにする。
    ///        使用後は解放してメモリを節約する。
    FrameBuffer initial_frame_buf_;
    FrameMeta   initial_frame_meta_{};
};

// ─── MonitorSupervisor ─────────────────────────────────────

/**
 * @brief すべてのモニターのパイプラインを管理するスーパーバイザー
 *
 * モニターの接続・切断・解像度変更を定期的に検知し、
 * 各モニターに対応する ScreenPipeline を生成・破棄する。
 * メトリクスの定期書き込みも担当する。
 */
class MonitorSupervisor {
public:
    /** @brief コンストラクタ */
    MonitorSupervisor();

    /** @brief デストラクタ */
    ~MonitorSupervisor();

    /**
     * @brief スーパーバイザーを初期化する
     * @param config  アプリケーション設定
     * @param error   エラーメッセージ出力先
     * @return 成功した場合 true
     */
    bool init(const AppConfig& config, std::string& error);

    /**
     * @brief メインループを実行する
     *
     * Ctrl+C などの停止シグナルが来るまで返らない。
     */
    void run();

    /**
     * @brief 停止リクエストを送る
     *
     * run() のループを終了させる。スレッドセーフ。
     */
    void request_stop();

private:
    /**
     * @brief モニター変化ポーリングスレッドのメイン関数
     *
     * 定期的に MonitorDetector::enumerate() を呼び出して差分を適用する。
     */
    void monitor_poll_loop();

    /**
     * @brief メトリクス書き込みスレッドのメイン関数
     *
     * 定期的に MetricsStore::save_metrics() / save_health() を呼び出す。
     */
    void metrics_writer_loop();

    /**
     * @brief 現在のモニター一覧と前回の一覧を比較して差分を適用する
     *
     * 追加・削除・解像度変更を検知し、対応するパイプラインを操作する。
     *
     * @param current 現在のモニター一覧
     */
    void apply_monitor_changes(const std::vector<MonitorInfo>& current);

    AppConfig config_;                          ///< アプリケーション設定
    std::shared_ptr<LogSink>      log_;         ///< ログシンク
    std::shared_ptr<MetricsStore> metrics_;     ///< メトリクスストア

    std::map<int, std::unique_ptr<ScreenPipeline>> pipelines_;  ///< モニター番号→パイプラインのマップ
    std::mutex pipelines_mutex_;                                  ///< パイプラインマップ保護用 mutex

    std::vector<MonitorInfo> last_monitors_;    ///< 前回のモニター一覧

    std::thread monitor_thread_;               ///< モニターポーリングスレッド
    std::thread metrics_thread_;               ///< メトリクス書き込みスレッド
    std::atomic<bool> stop_requested_{false};  ///< 停止リクエストフラグ
};

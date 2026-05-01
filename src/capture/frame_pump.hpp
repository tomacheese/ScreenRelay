#pragma once
#include "common/types.hpp"
#include "capture/capture_backend.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

/**
 * @brief バックグラウンドスレッドでフレームを取得し、境界キューに格納するクラス
 *
 * ICaptureBackend からフレームを継続的に取得し、内部キューに蓄積する。
 * コンシューマースレッドは try_pop / wait_pop でフレームを取り出す。
 * キューが MAX_QUEUE_SIZE を超えた場合は最古フレームを破棄する。
 */
class FramePump {
public:
    /**
     * @brief コンストラクタ
     *
     * スレッドは start() 呼び出し時に開始する。
     */
    FramePump();

    /**
     * @brief デストラクタ
     *
     * 実行中の場合は stop() を呼び出してスレッドを結合する。
     */
    ~FramePump();

    /**
     * @brief キャプチャを開始する
     *
     * バックグラウンドスレッドを起動して backend からフレームの取得を開始する。
     * 既に実行中の場合は何もしない。
     *
     * @param backend          キャプチャバックエンド (所有権なし。呼び出し元が管理する)
     * @param frame_timeout_ms フレーム取得タイムアウト (ms)
     */
    void start(ICaptureBackend* backend, int frame_timeout_ms);

    /**
     * @brief キャプチャを停止し、スレッドを結合する
     *
     * 既に停止している場合は何もしない。
     */
    void stop();

    /**
     * @brief フレームをノンブロッキングで取得する
     *
     * キューにフレームがある場合のみ buf と meta に書き込んで true を返す。
     * キューが空の場合は即座に false を返す。
     *
     * @param[out] buf   取得したフレームバッファ
     * @param[out] meta  取得したフレームメタデータ
     * @return キューからフレームを取得できた場合 true
     */
    bool try_pop(FrameBuffer& buf, FrameMeta& meta);

    /**
     * @brief フレームをタイムアウト付きで取得する
     *
     * キューにフレームが到着するまで最大 timeout_ms ミリ秒待機する。
     *
     * @param[out] buf        取得したフレームバッファ
     * @param[out] meta       取得したフレームメタデータ
     * @param      timeout_ms タイムアウト (ms)
     * @return フレームを取得できた場合 true、タイムアウトなら false
     */
    bool wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms);

    /**
     * @brief キャプチャスレッドが実行中かどうかを返す
     * @return 実行中なら true
     */
    bool is_running() const { return running_.load(); }

private:
    /**
     * @brief バックグラウンドキャプチャスレッドのエントリポイント
     *
     * running_ が false になるまでフレームを取得し続ける。
     */
    void capture_thread_func();

    /** キューの最大サイズ。超過した場合は最古フレームを破棄する */
    static constexpr int MAX_QUEUE_SIZE = 4;

    ICaptureBackend* backend_        = nullptr;   ///< キャプチャバックエンド (所有権なし)
    int              frame_timeout_ms_ = 100;     ///< フレーム取得タイムアウト (ms)

    std::atomic<bool>    running_{false};          ///< スレッド実行フラグ
    mutable std::mutex   mutex_;                   ///< キュー保護ミューテックス
    std::condition_variable cv_;                   ///< フレーム到着通知用条件変数
    std::queue<std::pair<FrameBuffer, FrameMeta>> queue_;  ///< フレームキュー
    std::thread thread_;                           ///< バックグラウンドキャプチャスレッド
    uint64_t    frame_seq_ = 0;                    ///< フレームシーケンスカウンター
};

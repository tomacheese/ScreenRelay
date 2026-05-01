#include "capture/frame_pump.hpp"
#include "common/time_utils.hpp"
#include <chrono>
#include <utility>

// ---------------------------------------------------------------------------
// コンストラクタ / デストラクタ
// ---------------------------------------------------------------------------

/**
 * @brief コンストラクタ
 *
 * メンバー変数をデフォルト初期化するのみ。スレッドは start() で起動する。
 */
FramePump::FramePump() = default;

/**
 * @brief デストラクタ
 *
 * 実行中の場合は stop() を呼び出してスレッドを安全に結合する。
 */
FramePump::~FramePump() {
    stop();
}

// ---------------------------------------------------------------------------
// 公開メソッド
// ---------------------------------------------------------------------------

/**
 * @brief キャプチャを開始する
 * @param backend          キャプチャバックエンド (所有権なし)
 * @param frame_timeout_ms フレーム取得タイムアウト (ms)
 */
void FramePump::start(ICaptureBackend* backend, int frame_timeout_ms) {
    // 既に実行中なら何もしない
    if (running_.load()) {
        return;
    }

    backend_          = backend;
    frame_timeout_ms_ = frame_timeout_ms;
    running_.store(true);

    thread_ = std::thread(&FramePump::capture_thread_func, this);
}

/**
 * @brief キャプチャを停止し、スレッドを結合する
 *
 * running_ フラグを false にしてスレッドの終了を待つ。
 * 既に停止している場合は何もしない。
 */
void FramePump::stop() {
    running_.store(false);

    // 条件変数で待機中のコンシューマーを起こす
    cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

/**
 * @brief フレームをノンブロッキングで取得する
 *
 * キューにフレームがある場合のみ取り出して true を返す。
 *
 * @param[out] buf   取得したフレームバッファ
 * @param[out] meta  取得したフレームメタデータ
 * @return キューからフレームを取得できた場合 true
 */
bool FramePump::try_pop(FrameBuffer& buf, FrameMeta& meta) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (queue_.empty()) {
        return false;
    }

    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();

    return true;
}

/**
 * @brief フレームをタイムアウト付きで取得する
 *
 * キューにフレームが到着するまで最大 timeout_ms ミリ秒待機する。
 *
 * @param[out] buf        取得したフレームバッファ
 * @param[out] meta       取得したフレームメタデータ
 * @param      timeout_ms タイムアウト (ms)
 * @return フレームを取得できた場合 true
 */
bool FramePump::wait_pop(FrameBuffer& buf, FrameMeta& meta, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    // キューが空かつ実行中の間は待機する
    bool notified = cv_.wait_until(lk, deadline, [this]() {
        return !queue_.empty() || !running_.load();
    });

    if (!notified || queue_.empty()) {
        return false;
    }

    auto& front = queue_.front();
    buf  = std::move(front.first);
    meta = std::move(front.second);
    queue_.pop();

    return true;
}

// ---------------------------------------------------------------------------
// プライベートメソッド
// ---------------------------------------------------------------------------

/**
 * @brief バックグラウンドキャプチャスレッドのエントリポイント
 *
 * running_ が false になるまでループし、backend_ からフレームを取得して
 * キューに格納する。キューが MAX_QUEUE_SIZE を超えた場合は最古フレームを破棄する。
 * acquire_frame() が内部でタイムアウト待機を行うため、追加のスリープは不要。
 */
void FramePump::capture_thread_func() {
    while (running_.load()) {
        // フレームを取得する。acquire_frame は frame_timeout_ms_ 分待機する
        auto opt = backend_->acquire_frame(frame_timeout_ms_);

        if (opt.has_value()) {
            FrameBuffer fb = std::move(*opt);

            // フレームメタデータを構築する
            FrameMeta meta;
            meta.sequence     = frame_seq_++;
            meta.width        = fb.width;
            meta.height       = fb.height;
            meta.timestamp_us = time_utils::system_now_us();

            {
                std::lock_guard<std::mutex> lk(mutex_);

                // キューが満杯の場合は最古フレームを破棄する
                if (static_cast<int>(queue_.size()) >= MAX_QUEUE_SIZE) {
                    queue_.pop();
                }

                queue_.emplace(std::move(fb), std::move(meta));
            }

            // コンシューマーにフレーム到着を通知する
            cv_.notify_one();
        }
        // acquire_frame がタイムアウトした場合は追加のスリープなしに次のループへ進む
    }
}

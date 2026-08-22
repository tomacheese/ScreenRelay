#pragma once
#include "audio/audio_capture.hpp"
#include "audio/audio_encoder.hpp"
#include "common/types.hpp"
#include "logging/log_sink.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief 音声パケットを 1 コンシューマー分だけ保持する境界キュー
 *
 * AudioPipeline は 1 つの共有音声ストリームを複数の ScreenPipeline へ配信
 * (ブロードキャスト) するため、購読者ごとに独立したキューを持たせる。
 * 各 ScreenPipeline は自身の RTSP 接続状況に応じて任意のタイミングで
 * try_pop / wait_pop できる。
 */
class AudioSubscriberQueue {
public:
    /**
     * @brief パケットをキューへ追加する
     *
     * キューが最大サイズを超える場合は最古パケットを破棄する
     * (購読側の消費が追いつかない場合でも配信スレッドをブロックしないため)。
     *
     * @param pkt 追加するパケット
     */
    void push(const EncodedPacket& pkt);

    /**
     * @brief パケットをノンブロッキングで取得する
     * @param[out] out 取得したパケット
     * @return 取得できた場合 true
     */
    bool try_pop(EncodedPacket& out);

    /**
     * @brief パケットをタイムアウト付きで取得する
     * @param[out] out       取得したパケット
     * @param      timeout_ms タイムアウト (ms)
     * @return 取得できた場合 true、タイムアウトなら false
     */
    bool wait_pop(EncodedPacket& out, int timeout_ms);

private:
    /** キューの最大サイズ。超過した場合は最古パケットを破棄する */
    static constexpr int kMaxQueueSize = 64;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<EncodedPacket> queue_;
};

/**
 * @brief 共有音声キャプチャ・エンコードパイプライン
 *
 * WasapiAudioCapture でキャプチャした PCM を AudioEncoderController で AAC に
 * エンコードし、購読中のすべての AudioSubscriberQueue へブロードキャストする。
 * 全モニターの RTSP ストリームに同一の音声を配信するため、モニター数に関係なく
 * インスタンスは 1 つだけ生成される。
 */
class AudioPipeline {
public:
    /** @brief コンストラクタ */
    AudioPipeline();

    /** @brief デストラクタ。実行中の場合は stop() を呼び出す */
    ~AudioPipeline();

    /**
     * @brief キャプチャ・エンコードを初期化してバックグラウンドスレッドを開始する
     * @param config 音声設定
     * @param error  エラーメッセージ出力先
     * @param log    エンコード失敗などをログ出力するためのシンク (nullptr の場合はログを出力しない)
     * @return 成功した場合 true
     */
    bool start(const AudioConfig& config, std::string& error,
               std::shared_ptr<LogSink> log = nullptr);

    /** @brief バックグラウンドスレッドを停止してリソースを解放する */
    void stop();

    /** @brief パイプラインが実行中かどうかを返す */
    bool is_running() const { return running_.load(); }

    /**
     * @brief RTSP ストリーム初期化用のコーデック情報を取得する
     *
     * start() が成功した後にのみ有効な値を返す。
     *
     * @return 音声コーデック情報
     */
    AudioCodecInfo get_codec_info() const;

    /**
     * @brief 新しい購読者を登録する
     * @return 購読用キュー (ScreenPipeline が保持し、pop に使用する)
     */
    std::shared_ptr<AudioSubscriberQueue> subscribe();

    /**
     * @brief 購読を解除する
     * @param queue subscribe() で取得したキュー
     */
    void unsubscribe(const std::shared_ptr<AudioSubscriberQueue>& queue);

private:
    /** @brief バックグラウンドキャプチャ・エンコードスレッドのエントリポイント */
    void pipeline_thread_func();

    WasapiAudioCapture     capture_;  ///< WASAPI キャプチャバックエンド
    AudioEncoderController encoder_;  ///< AAC エンコーダー
    std::shared_ptr<LogSink> log_;    ///< エンコード失敗等の記録先 (nullptr 可)

    std::atomic<bool> running_{false};
    std::thread        thread_;

    mutable std::mutex codec_info_mutex_;
    AudioCodecInfo      codec_info_;

    mutable std::mutex subscribers_mutex_;
    std::vector<std::shared_ptr<AudioSubscriberQueue>> subscribers_;
};

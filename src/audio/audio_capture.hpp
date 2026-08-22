#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief WASAPI を使用した音声キャプチャバックエンド
 *
 * source == "loopback" の場合はレンダリングデバイス（既定 or 指定デバイス）が
 * 再生している音声を IAudioClient のループバックモードで取得する。
 * source == "capture" の場合は録音デバイス（マイク等）から直接取得する。
 *
 * バックグラウンドスレッドがイベント駆動でバッファを取得し、内部の境界キューに
 * 格納する。コンシューマースレッドは try_pop / wait_pop で取り出す。
 */
class WasapiAudioCapture {
public:
    /** @brief コンストラクタ */
    WasapiAudioCapture();

    /** @brief デストラクタ。release() を呼び出してリソースを解放する */
    ~WasapiAudioCapture();

    /**
     * @brief 利用可能な音声デバイスを列挙する
     *
     * レンダリングデバイス（ループバック用）と録音デバイスの両方を返す。
     *
     * @param error エラーメッセージ出力先
     * @return デバイス情報のリスト（失敗時は空）
     */
    static std::vector<AudioDeviceInfo> enumerate_devices(std::string& error);

    /**
     * @brief 音声キャプチャを初期化してバックグラウンドスレッドを開始する
     *
     * @param config 音声設定（source / device_id を参照する）
     * @param error  エラーメッセージ出力先
     * @return 成功した場合 true
     */
    bool init(const AudioConfig& config, std::string& error);

    /** @brief キャプチャデバイスの実際のサンプルレート (Hz) を返す */
    int sample_rate() const { return sample_rate_; }

    /** @brief キャプチャデバイスの実際のチャンネル数を返す */
    int channels() const { return channels_; }

    /** @brief キャプチャデバイスのサンプルフォーマットが IEEE float かどうかを返す */
    bool is_float() const { return is_float_; }

    /** @brief キャプチャデバイスの 1 サンプルあたりのビット数を返す */
    int bits_per_sample() const { return bits_per_sample_; }

    /**
     * @brief バッファをノンブロッキングで取得する
     * @param[out] buf  取得したバッファ
     * @param[out] meta 取得したメタデータ
     * @return 取得できた場合 true
     */
    bool try_pop(AudioBuffer& buf, AudioMeta& meta);

    /**
     * @brief バッファをタイムアウト付きで取得する
     * @param[out] buf        取得したバッファ
     * @param[out] meta       取得したメタデータ
     * @param      timeout_ms タイムアウト (ms)
     * @return 取得できた場合 true、タイムアウトなら false
     */
    bool wait_pop(AudioBuffer& buf, AudioMeta& meta, int timeout_ms);

    /**
     * @brief 回復不能なキャプチャエラーが発生しているか返す
     * @return エラー発生時 true
     */
    bool has_hard_error() const { return hard_error_.load(); }

    /**
     * @brief 最後のエラーメッセージを返す
     * @return エラーメッセージ文字列のコピー
     */
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(last_error_mutex_);
        return last_error_;
    }

    /**
     * @brief キャプチャスレッドを停止してすべてのリソースを解放する
     *
     * 二重呼び出しに対して安全。
     */
    void release();

private:
    /** @brief バックグラウンドキャプチャスレッドのエントリポイント */
    void capture_thread_func();

    /** キューの最大サイズ。超過した場合は最古バッファを破棄する */
    static constexpr int kMaxQueueSize = 32;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<AudioBuffer, AudioMeta>> queue_;

    std::atomic<bool> hard_error_{false};
    mutable std::mutex last_error_mutex_;
    std::string        last_error_;

    int  sample_rate_     = 48000;
    int  channels_        = 2;
    bool is_float_        = true;
    int  bits_per_sample_ = 32;
};

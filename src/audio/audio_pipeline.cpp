#include "audio/audio_pipeline.hpp"
#include <algorithm>
#include <chrono>

void AudioSubscriberQueue::push(const EncodedPacket& pkt) {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.push(pkt);
    while (static_cast<int>(queue_.size()) > kMaxQueueSize) {
        queue_.pop();
    }
    cv_.notify_one();
}

bool AudioSubscriberQueue::try_pop(EncodedPacket& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    return true;
}

bool AudioSubscriberQueue::wait_pop(EncodedPacket& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty(); })) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop();
    return true;
}

AudioPipeline::AudioPipeline() = default;

AudioPipeline::~AudioPipeline() { stop(); }

bool AudioPipeline::start(const AudioConfig& config, std::string& error,
                          std::shared_ptr<LogSink> log) {
    if (running_.load()) return true;

    log_ = std::move(log);

    if (!capture_.init(config, error)) {
        return false;
    }

    if (!encoder_.init(config, capture_.sample_rate(), capture_.channels(),
                       capture_.is_float(), error)) {
        capture_.release();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(codec_info_mutex_);
        codec_info_ = encoder_.get_codec_info();
    }

    running_.store(true);
    thread_ = std::thread(&AudioPipeline::pipeline_thread_func, this);
    return true;
}

void AudioPipeline::stop() {
    // running_ はハードエラー発生時にスレッド自身が false へ落とすことがあるため、
    // ここでの判定に使わず thread_.joinable() で二重解放を防ぐ。
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    encoder_.reset();
    capture_.release();
}

AudioCodecInfo AudioPipeline::get_codec_info() const {
    std::lock_guard<std::mutex> lk(codec_info_mutex_);
    return codec_info_;
}

std::shared_ptr<AudioSubscriberQueue> AudioPipeline::subscribe() {
    auto queue = std::make_shared<AudioSubscriberQueue>();
    std::lock_guard<std::mutex> lk(subscribers_mutex_);
    subscribers_.push_back(queue);
    return queue;
}

void AudioPipeline::unsubscribe(const std::shared_ptr<AudioSubscriberQueue>& queue) {
    std::lock_guard<std::mutex> lk(subscribers_mutex_);
    subscribers_.erase(
        std::remove(subscribers_.begin(), subscribers_.end(), queue),
        subscribers_.end());
}

void AudioPipeline::pipeline_thread_func() {
    AudioBuffer buf;
    AudioMeta   meta;
    std::vector<EncodedPacket> packets;
    bool encode_failing = false;  // 連続失敗中はログを 1 度だけ出し、復帰時にも 1 度だけ記録する

    while (running_.load()) {
        if (capture_.has_hard_error()) {
            // キャプチャデバイスが失われた場合、音声配信のみ停止して再試行はしない。
            // 映像パイプラインへの影響を避けるため、ここではプロセス全体を落とさない。
            // running_ を false にしておかないと is_running() が true のまま残り、
            // 呼び出し側が「音声配信中」と誤認して RTSP に存在しないストリームを
            // 告知し続けてしまう。
            running_.store(false);
            break;
        }

        if (!capture_.wait_pop(buf, meta, 200)) {
            continue;
        }

        packets.clear();
        if (!encoder_.encode(buf, meta, packets)) {
            if (!encode_failing && log_) {
                log_->log_error("AUDIO_ENCODE_FAILED",
                                 "Audio encode failed, dropping buffer");
            }
            encode_failing = true;
            continue;
        }
        if (encode_failing) {
            if (log_) {
                log_->log_event(spdlog::level::info, "audio_encode_recovered");
            }
            encode_failing = false;
        }
        if (packets.empty()) continue;

        std::lock_guard<std::mutex> lk(subscribers_mutex_);
        for (const auto& subscriber : subscribers_) {
            for (const auto& pkt : packets) {
                subscriber->push(pkt);
            }
        }
    }
}

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
 * @brief Bounded packet queue for a single audio consumer.
 *
 * AudioPipeline broadcasts one shared audio stream to several ScreenPipelines, so each
 * subscriber gets its own queue and can pop at its own pace, depending on its RTSP
 * connection state.
 */
class AudioSubscriberQueue {
public:
    /**
     * @brief Appends a packet, dropping the oldest on overflow so a slow subscriber
     *        never blocks the pipeline thread.
     */
    void push(const EncodedPacket& pkt);

    /**
     * @brief Non-blocking pop.
     * @param[out] out Popped packet
     * @return true if a packet was available
     */
    bool try_pop(EncodedPacket& out);

    /**
     * @brief Pops a packet, waiting up to timeout_ms.
     * @param[out] out       Popped packet
     * @param      timeout_ms Timeout (ms)
     * @return true if a packet was popped, false on timeout
     */
    bool wait_pop(EncodedPacket& out, int timeout_ms);

private:
    /** Queue capacity; the oldest packet is dropped on overflow. */
    static constexpr int kMaxQueueSize = 64;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<EncodedPacket> queue_;
};

/**
 * @brief Shared audio capture and encode pipeline.
 *
 * Encodes PCM captured by WasapiAudioCapture to AAC with AudioEncoderController and
 * broadcasts it to every subscribed AudioSubscriberQueue. All monitors' RTSP streams
 * carry the same audio, so only one instance exists regardless of monitor count.
 */
class AudioPipeline {
public:
    AudioPipeline();

    ~AudioPipeline();

    /**
     * @brief Initializes capture and encoder and starts the pipeline thread.
     *        Returns true without doing anything if already running.
     * @param config Audio settings
     * @param error  Receives an error message on failure
     * @param log    Sink for encode failures etc. (nullptr disables logging)
     * @return true on success
     */
    bool start(const AudioConfig& config, std::string& error,
               std::shared_ptr<LogSink> log = nullptr);

    /** @brief Stops the thread and releases capture/encoder resources. */
    void stop();

    /** @brief Whether the pipeline is running; becomes false if capture hits a hard error. */
    bool is_running() const { return running_.load(); }

    /** @brief Codec parameters for RTSP stream setup; valid only after a successful start(). */
    AudioCodecInfo get_codec_info() const;

    /** @brief Registers a subscriber and returns its queue (held by the ScreenPipeline). */
    std::shared_ptr<AudioSubscriberQueue> subscribe();

    /** @brief Removes a subscriber; @p queue is the value returned by subscribe(). */
    void unsubscribe(const std::shared_ptr<AudioSubscriberQueue>& queue);

private:
    void pipeline_thread_func();

    WasapiAudioCapture     capture_;  ///< WASAPI capture backend
    AudioEncoderController encoder_;  ///< AAC encoder
    std::shared_ptr<LogSink> log_;    ///< Encode failure sink (may be null)

    std::atomic<bool> running_{false};
    std::thread        thread_;

    mutable std::mutex codec_info_mutex_;
    AudioCodecInfo      codec_info_;

    mutable std::mutex subscribers_mutex_;
    std::vector<std::shared_ptr<AudioSubscriberQueue>> subscribers_;
};

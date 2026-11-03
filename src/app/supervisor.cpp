#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "app/supervisor.hpp"
#include "common/time_utils.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

// ════════════════════════════════════════════════════════════
// ScreenPipeline
// ════════════════════════════════════════════════════════════

ScreenPipeline::ScreenPipeline(const MonitorInfo& info, const AppConfig& config,
                                std::shared_ptr<LogSink> log,
                                std::shared_ptr<MetricsStore> metrics,
                                std::shared_ptr<AudioPipeline> audio_pipeline)
    : monitor_info_(info)
    , config_(config)
    , log_(std::move(log))
    , metrics_(std::move(metrics))
    , audio_pipeline_(std::move(audio_pipeline)) {
    state_machine_.on_transition([this](PipelineState from, PipelineState to) {
        log_->log_state_changed(monitor_info_.number,
                                state_name(from), state_name(to));
        metrics_->set_state(monitor_info_.number, state_name(to));
    });

    // Subscribe to the shared audio pipeline (same audio goes to every monitor's
    // streams); the subscription lasts for this pipeline's lifetime.
    if (audio_pipeline_) {
        audio_queue_ = audio_pipeline_->subscribe();
    }
}

ScreenPipeline::~ScreenPipeline() {
    stop();
    if (audio_pipeline_ && audio_queue_) {
        audio_pipeline_->unsubscribe(audio_queue_);
    }
}

void ScreenPipeline::start() {
    stop_requested_.store(false);
    pipeline_thread_ = std::thread(&ScreenPipeline::pipeline_thread_func, this);
}

void ScreenPipeline::stop() {
    stop_requested_.store(true);
    if (pipeline_thread_.joinable())
        pipeline_thread_.join();
}

void ScreenPipeline::notify_resized(int new_width, int new_height) {
    {
        std::lock_guard<std::mutex> lk(resize_mutex_);
        pending_resize_ = {new_width, new_height};
    }
    resize_pending_.store(true);
}

/**
 * @brief Init the capture backend with a bounded retry loop.
 *
 * Right after a resolution change, or when another monitor on the same GPU is
 * reconfiguring, DuplicateOutput can fail with E_ACCESSDENIED (0x80070005)
 * while the display driver settles; it usually succeeds on the 2nd retry.
 * Aborts early once stop_requested_ is set.
 */
bool ScreenPipeline::init_capture_with_retry(ICaptureBackend* backend,
                                               int width, int height) {
    constexpr int kMaxRetries   = 15;
    constexpr int kRetryDelayMs = 200;
    for (int retry = 0; retry < kMaxRetries && !stop_requested_.load(); ++retry) {
        if (retry > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
        if (backend->init(monitor_info_.handle, width, height))
            return true;
    }
    return false;
}

/** Replaces {n} in path_pattern with the monitor number; the primary monitor also gets a number-0 alias. */
std::vector<std::string> ScreenPipeline::make_rtsp_urls() const {
    auto make_url = [&](int n) {
        std::string path = config_.rtsp.path_pattern;
        auto pos = path.find("{n}");
        if (pos != std::string::npos)
            path.replace(pos, 3, std::to_string(n));
        return config_.rtsp.base_url + path;
    };

    std::vector<std::string> urls;
    urls.push_back(make_url(monitor_info_.number));
    if (monitor_info_.is_primary)
        urls.push_back(make_url(0));
    return urls;
}

/**
 * @brief Init DXGI capture and FramePump, then wait for the first frame (INIT -> CAPTURING).
 *
 * Any failure moves the pipeline to FATAL (or leaves state unchanged if the
 * CAPTURING transition itself is rejected) and returns false.
 */
bool ScreenPipeline::do_init_and_capture() {
    auto backend = std::make_unique<DxgiCaptureBackend>();
    if (!init_capture_with_retry(backend.get(),
                                  monitor_info_.logical_width,
                                  monitor_info_.logical_height)) {
        log_->log_error("DXGI_INIT_FAILED",
                        "DxgiCaptureBackend::init failed for monitor "
                        + std::to_string(monitor_info_.number)
                        + ": " + backend->last_error());
        if (!state_machine_.transition_to(PipelineState::FATAL)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to FATAL from "
                            + std::string(state_name(state_machine_.current_state())));
        }
        return false;
    }
    capture_backend_ = std::move(backend);

    frame_pump_ = std::make_unique<FramePump>();
    frame_pump_->start(capture_backend_.get(), config_.capture.frame_timeout_ms);

    if (!state_machine_.transition_to(PipelineState::CAPTURING)) {
        log_->log_error("STATE_TRANSITION_FAILED", "Cannot transition to CAPTURING");
        frame_pump_->stop();
        capture_backend_->release();
        return false;
    }

    // Wait for the first frame to learn the actual resolution. It can be slow
    // under load, hence the generous timeout (frame_timeout_ms * 100).
    FrameBuffer buf;
    FrameMeta   meta;
    bool got_frame = frame_pump_->wait_pop(buf, meta,
                                            config_.capture.frame_timeout_ms * 100);
    if (!got_frame) {
        log_->log_error("FIRST_FRAME_TIMEOUT",
                        "Timeout waiting for first frame from monitor "
                        + std::to_string(monitor_info_.number));
        if (!state_machine_.transition_to(PipelineState::FATAL)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to FATAL");
        }
        frame_pump_->stop();
        capture_backend_->release();
        return false;
    }

    current_width_  = static_cast<int>(buf.width);
    current_height_ = static_cast<int>(buf.height);
    metrics_->set_resolution(monitor_info_.number, current_width_, current_height_);

    // Keep it as the initial freeze frame: Desktop Duplication returns no new
    // frame until the screen changes, so on an idle monitor the stream would
    // otherwise stay black. This frame was captured before the switch to GPU
    // zero-copy mode, so gpu_texture is null; encode_gpu_zero_copy() handles
    // that via UpdateSubresource.
    initial_frame_buf_  = std::move(buf);
    initial_frame_meta_ = meta;

    return true;
}

/**
 * @brief Init the encoder and RTSP clients and connect (CAPTURING -> CONNECTING -> STREAMING).
 *
 * Encoder init failure goes to FATAL; RTSP connect failure goes back to
 * CAPTURING so the caller can retry.
 */
bool ScreenPipeline::do_connect() {
    if (!state_machine_.transition_to(PipelineState::CONNECTING)) {
        log_->log_error("STATE_TRANSITION_FAILED", "Cannot transition to CONNECTING");
        return false;
    }

    // Share the D3D11 device with the encoder only if the backend supports
    // zero-copy (physical resolution == logical resolution, no scaling needed).
    void* shared_d3d11_device = nullptr;
    if (capture_backend_->supports_zero_copy()) {
        shared_d3d11_device = capture_backend_->gpu_device();
    }

    encoder_ = std::make_unique<EncoderController>();
    std::string enc_err;
    if (!encoder_->init(config_.encoder,
                        static_cast<uint32_t>(current_width_),
                        static_cast<uint32_t>(current_height_),
                        enc_err,
                        shared_d3d11_device)) {
        log_->log_error("ENCODER_INIT_FAILED", enc_err);
        encoder_.reset();
        if (!state_machine_.transition_to(PipelineState::FATAL)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to FATAL after encoder init failure");
        }
        return false;
    }

    // Switch capture to zero-copy mode only if the encoder established the
    // zero-copy path (skips CPU colour conversion and readback entirely).
    const bool gpu_zero_copy = encoder_->is_gpu_zero_copy_active();
    capture_backend_->set_zero_copy_mode(gpu_zero_copy);
    log_->log_event(spdlog::level::info, "gpu_zero_copy_mode",
                    {{"monitor", std::to_string(monitor_info_.number)},
                     {"active", gpu_zero_copy ? "true" : "false"}});

    const std::string& actual_codec = encoder_->selected_codec_name();
    log_->log_encoder_initialized(monitor_info_.number,
                                   actual_codec,
                                   current_width_, current_height_,
                                   config_.encoder.fps,
                                   config_.encoder.bitrate_kbps);
    metrics_->set_encoder_codec(monitor_info_.number, actual_codec);

    auto codec_info = encoder_->get_codec_info();
    // Fix the RTCP SR NTP base here. connect() would fall back to av_gettime()
    // when stream_start_us is 0, but audio pts recomputation needs the same base.
    const int64_t connect_wallclock_us = time_utils::system_now_us();
    codec_info.stream_start_us = connect_wallclock_us;
    auto urls = make_rtsp_urls();
    rtsp_clients_.clear();

    // If the shared audio pipeline is running, add its audio stream to every RTSP
    // stream; audio pts is computed relative to connect_wallclock_us to stay in sync with video.
    bool has_audio = (audio_pipeline_ && audio_pipeline_->is_running());
    AudioCodecInfo audio_info;
    if (has_audio) {
        audio_info = audio_pipeline_->get_codec_info();
        audio_stream_start_us_ = connect_wallclock_us;
        last_audio_pts_        = -1;
    }

    for (const auto& url : urls) {
        auto client = std::make_unique<RtspPublisherClient>();
        std::string rtsp_err;
        if (!client->connect(url, config_.rtsp, codec_info, rtsp_err,
                             has_audio ? &audio_info : nullptr)) {
            log_->log_error("RTSP_CONNECT_FAILED", rtsp_err,
                            {{"url", url},
                             {"monitor", std::to_string(monitor_info_.number)}});
            rtsp_clients_.clear();
            encoder_.reset();
            if (!state_machine_.transition_to(PipelineState::CAPTURING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to CAPTURING after RTSP connect failure");
            }
            return false;
        }

        log_->log_publish_started(monitor_info_.number, url,
                                   current_width_, current_height_,
                                   config_.encoder.fps);
        rtsp_clients_.push_back(std::move(client));
    }

    if (!state_machine_.transition_to(PipelineState::STREAMING)) {
        log_->log_error("STATE_TRANSITION_FAILED", "Cannot transition to STREAMING");
        rtsp_clients_.clear();
        encoder_.reset();
        return false;
    }

    rtsp_backoff_.reset(config_.rtsp.reconnect_delay_ms);
    return true;
}

/**
 * @brief Encode/send loop, run while in STREAMING.
 *
 * Desktop Duplication delivers no frame while the screen is static, so the
 * last received frame is re-sent at the configured fps (freeze frame).
 * Exits on stop, RTSP send error (-> RECONNECTING), encode failure (-> FATAL),
 * a pending resize or a DXGI hard error such as ACCESS_LOST (-> RECONFIGURING).
 */
void ScreenPipeline::do_streaming_loop() {
    time_utils::Stopwatch metrics_sw;
    uint64_t bytes_since_last  = 0;
    uint64_t frames_since_last = 0;

    // Cache of the last frame, re-sent on a static screen. Seeded with the saved
    // initial frame because on an idle monitor DXGI can take a very long time to
    // return the next frame, which would leave the stream black.
    FrameBuffer freeze_buf  = std::move(initial_frame_buf_);
    FrameMeta   freeze_meta = initial_frame_meta_;
    bool        has_freeze  = (!freeze_buf.data.empty() || freeze_buf.gpu_texture != nullptr);
    initial_frame_meta_ = {};

    // Whether the pixels differ from the previously encoded frame. Set when a new
    // frame arrives; a re-sent freeze frame keeps it false so encode() can skip
    // the sws_scale colour conversion.
    bool content_changed = true;

    const int64_t frame_interval_us = 1000000LL / config_.encoder.fps;
    time_utils::Stopwatch frame_sw;  // time since the last frame was sent

    while (!stop_requested_.load() &&
           state_machine_.current_state() == PipelineState::STREAMING) {

        // A hard error (e.g. ACCESS_LOST) triggers an immediate re-init. A mere
        // timeout (static screen) is not an error, so it never causes RECONFIGURING.
        if (frame_pump_->has_backend_error()) {
            log_->log_error("DXGI_HARD_ERROR",
                            "DXGI hard error detected for monitor "
                            + std::to_string(monitor_info_.number)
                            + ": " + capture_backend_->last_error());
            if (!state_machine_.transition_to(PipelineState::RECONFIGURING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to RECONFIGURING from STREAMING");
            }
            return;
        }

        // Drain audio packets every iteration, capped per iteration:
        // av_interleaved_write_frame is a synchronous network call, so an
        // unbounded drain of a backlog would disturb video frame pacing.
        if (audio_queue_) {
            EncodedPacket apkt;
            constexpr int kMaxAudioPacketsPerLoop = 16;
            for (int i = 0; i < kMaxAudioPacketsPerLoop && audio_queue_->try_pop(apkt); ++i) {
                // Convert the absolute capture time to samples since this
                // connection's NTP base (audio_stream_start_us_); keep pts strictly increasing.
                int64_t rel_us = apkt.capture_timestamp_us - audio_stream_start_us_;
                if (rel_us < 0) rel_us = 0;
                int64_t pts = (rel_us * apkt.time_base_den) / 1000000LL;
                if (pts <= last_audio_pts_) pts = last_audio_pts_ + 1;
                last_audio_pts_ = pts;
                apkt.pts = pts;
                apkt.dts = pts;

                for (auto& client : rtsp_clients_) {
                    if (client->has_audio_stream()) {
                        if (!client->send_audio_packet(apkt)) {
                            log_->log_error("RTSP_SEND_FAILED",
                                            "Audio packet send failed for monitor "
                                            + std::to_string(monitor_info_.number));
                        }
                    }
                }
            }
        }

        // Wait at most until the next frame is due
        int64_t wait_us = frame_interval_us - frame_sw.elapsed_us();
        int     wait_ms = (wait_us > 0)
                          ? std::min(static_cast<int>(wait_us / 1000) + 1,
                                     config_.capture.frame_timeout_ms * 2)
                          : 0;

        FrameBuffer buf;
        FrameMeta   meta;
        bool got_frame = (wait_ms > 0)
                         ? frame_pump_->wait_pop(buf, meta, wait_ms)
                         : frame_pump_->try_pop(buf, meta);

        if (got_frame) {
            freeze_buf       = std::move(buf);
            freeze_meta      = meta;
            has_freeze       = true;
            content_changed  = true;
            metrics_->increment_frames_received(monitor_info_.number);
        }

        // Not yet due (new frames too): never send faster than the configured fps
        if (frame_sw.elapsed_us() < frame_interval_us) {
            continue;
        }

        // No frame received yet
        if (!has_freeze) {
            continue;
        }

        // Stamp every sent frame (new or freeze) with the current wall-clock time
        freeze_meta.timestamp_us = time_utils::system_now_us();

        frame_sw.reset();

        if (resize_pending_.load()) {
            if (!state_machine_.transition_to(PipelineState::RECONFIGURING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to RECONFIGURING");
            }
            break;
        }

        std::vector<EncodedPacket> packets;
        if (!encoder_->encode(freeze_buf, freeze_meta, packets, content_changed)) {
            log_->log_error("ENCODER_ENCODE_FAILED",
                            "encode() returned false for monitor "
                            + std::to_string(monitor_info_.number));
            metrics_->increment_frames_dropped(monitor_info_.number);
            if (!state_machine_.transition_to(PipelineState::FATAL)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to FATAL after encode failure");
            }
            break;
        }
        // Unchanged until the next new frame arrives
        content_changed = false;

        bool rtsp_error = false;
        for (auto& pkt : packets) {
            for (auto& client : rtsp_clients_) {
                if (!client->send_packet(pkt)) {
                    log_->log_error("RTSP_SEND_FAILED",
                                    "send_packet failed for monitor "
                                    + std::to_string(monitor_info_.number));
                    metrics_->increment_rtsp_errors(monitor_info_.number);
                    rtsp_error = true;
                    break;
                }
            }
            if (rtsp_error) break;
        }

        if (rtsp_error) {
            if (!state_machine_.transition_to(PipelineState::RECONNECTING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to RECONNECTING");
            }
            break;
        }

        if (!packets.empty()) {
            metrics_->increment_frames_encoded(monitor_info_.number);
            frames_since_last++;
            for (const auto& pkt : packets) bytes_since_last += pkt.data.size();

            // Update fps/bitrate metrics about once per second
            int64_t elapsed_ms = metrics_sw.elapsed_ms();
            if (elapsed_ms >= 1000) {
                double fps  = frames_since_last * 1000.0 / elapsed_ms;
                double kbps = bytes_since_last  * 8.0   / elapsed_ms;
                metrics_->set_current_fps(monitor_info_.number, fps);
                metrics_->set_bitrate_kbps(monitor_info_.number, kbps);
                frames_since_last = 0;
                bytes_since_last  = 0;
                metrics_sw.reset();
            }
        }
    }

    // Save the last freeze frame for the next call. After RECONNECTING ->
    // STREAMING, initial_frame_buf_ would be empty (moved out above) and a static
    // monitor in zero-copy mode would stay black until DXGI returns a new frame.
    // A saved GPU texture must be cleared in do_reconfigure()/teardown_all()
    // before the D3D11 device is released.
    if (has_freeze) {
        initial_frame_buf_  = std::move(freeze_buf);
        initial_frame_meta_ = freeze_meta;
    }
}

/**
 * @brief Reconnect all RTSP streams with exponential backoff (RECONNECTING -> STREAMING).
 *
 * Moves to STOPPING if stop_requested_ is set before reconnection succeeds.
 */
void ScreenPipeline::do_reconnect() {
    // Log publish_stopped once for the group, not per client
    bool was_connected = std::any_of(rtsp_clients_.begin(), rtsp_clients_.end(),
                                     [](const auto& c) { return c->is_connected(); });
    if (was_connected) log_->log_publish_stopped(monitor_info_.number, "reconnecting");
    for (auto& client : rtsp_clients_) client->disconnect();
    rtsp_clients_.clear();

    while (!stop_requested_.load()) {
        metrics_->increment_reconnect_attempts(monitor_info_.number);
        int delay = rtsp_backoff_.next_delay(config_.rtsp.reconnect_max_delay_ms,
                                              config_.rtsp.reconnect_backoff_multiplier);
        log_->log_reconnect(monitor_info_.number, rtsp_backoff_.attempt, delay);

        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        if (stop_requested_.load()) break;

        auto urls = make_rtsp_urls();
        auto codec_info = encoder_->get_codec_info();
        // Use the first frame's time as the RTCP SR NTP base (not the reconnect
        // time) so receivers such as VLC do not see timestamps far from the encoder's.
        codec_info.stream_start_us = encoder_->first_frame_time_us();

        bool has_audio = (audio_pipeline_ && audio_pipeline_->is_running());
        AudioCodecInfo audio_info;
        if (has_audio) {
            audio_info = audio_pipeline_->get_codec_info();
            audio_stream_start_us_ = codec_info.stream_start_us;
            last_audio_pts_        = -1;
        }

        bool all_ok = true;
        std::vector<std::unique_ptr<RtspPublisherClient>> new_clients;

        for (const auto& url : urls) {
            auto client = std::make_unique<RtspPublisherClient>();
            std::string err;
            if (!client->connect(url, config_.rtsp, codec_info, err,
                                 has_audio ? &audio_info : nullptr)) {
                log_->log_error("RTSP_RECONNECT_FAILED", err,
                                {{"url", url},
                                 {"monitor", std::to_string(monitor_info_.number)}});
                all_ok = false;
                break;
            }
            log_->log_publish_started(monitor_info_.number, url,
                                       current_width_, current_height_,
                                       config_.encoder.fps);
            new_clients.push_back(std::move(client));
        }

        if (all_ok) {
            rtsp_clients_ = std::move(new_clients);
            rtsp_backoff_.reset(config_.rtsp.reconnect_delay_ms);
            if (!state_machine_.transition_to(PipelineState::STREAMING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to STREAMING after reconnect");
            }
            // Make the first frame after reconnecting an IDR so receivers can
            // start decoding immediately. The encoder is not reset, so PTS stays continuous.
            encoder_->request_next_idr();
            return;
        }
    }

    // Only reached when stop was requested
    if (!state_machine_.transition_to(PipelineState::STOPPING)) {
        log_->log_error("STATE_TRANSITION_FAILED",
                        "Cannot transition to STOPPING from RECONNECTING");
    }
}

/**
 * @brief Handle a resolution change or DXGI hard error (RECONFIGURING -> CAPTURING).
 *
 * Stops the FramePump, releases the encoder and RTSP clients, re-inits the DXGI
 * backend at the new size and returns to CAPTURING; do_connect() then
 * runs from pipeline_thread_func(). A failed DXGI re-init goes to FATAL.
 */
void ScreenPipeline::do_reconfigure() {
    if (frame_pump_) frame_pump_->stop();

    // Drop any saved frame holding a GPU texture before the backend (and its
    // D3D11 device) is released, so no texture of the old device outlives it.
    initial_frame_buf_  = FrameBuffer{};
    initial_frame_meta_ = {};

    bool was_connected = std::any_of(rtsp_clients_.begin(), rtsp_clients_.end(),
                                     [](const auto& c) { return c->is_connected(); });
    if (was_connected) log_->log_publish_stopped(monitor_info_.number, "reconfiguring");
    for (auto& client : rtsp_clients_) client->disconnect();
    rtsp_clients_.clear();

    if (encoder_) {
        std::vector<EncodedPacket> flush_pkts;
        encoder_->flush(flush_pkts);
        encoder_->reset();
        encoder_.reset();
    }

    // Consume the resize flag atomically, then read the size under the mutex
    bool had_resize = resize_pending_.exchange(false);
    PendingResize pending;
    if (had_resize) {
        std::lock_guard<std::mutex> lk(resize_mutex_);
        pending = pending_resize_;
    }
    int new_w = had_resize ? pending.width  : current_width_;
    int new_h = had_resize ? pending.height : current_height_;

    // Full DXGI re-init (also recovers from ACCESS_LOST); retried before being
    // treated as fatal, see init_capture_with_retry().
    if (capture_backend_) {
        capture_backend_->release();

        if (!init_capture_with_retry(capture_backend_.get(), new_w, new_h)) {
            log_->log_error("DXGI_REINIT_FAILED",
                            "Failed to reinit DXGI for monitor "
                            + std::to_string(monitor_info_.number)
                            + ": " + capture_backend_->last_error());
            if (!state_machine_.transition_to(PipelineState::FATAL)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to FATAL after DXGI reinit failure");
            }
            return;
        }
    }

    current_width_  = new_w;
    current_height_ = new_h;
    metrics_->set_resolution(monitor_info_.number, current_width_, current_height_);

    // Restart the FramePump and seed the initial freeze frame: the first
    // AcquireNextFrame after a fresh DuplicateOutput returns the current desktop
    // even if nothing changed, so video flows immediately instead of waiting for
    // the next frame (which can take a long time on an idle monitor).
    if (frame_pump_ && capture_backend_) {
        frame_pump_->start(capture_backend_.get(), config_.capture.frame_timeout_ms);

        FrameBuffer seed_fb;
        FrameMeta   seed_meta;
        if (frame_pump_->wait_pop(seed_fb, seed_meta, config_.capture.frame_timeout_ms)) {
            initial_frame_buf_  = std::move(seed_fb);
            initial_frame_meta_ = seed_meta;
        }
    }

    if (!state_machine_.transition_to(PipelineState::CAPTURING)) {
        log_->log_error("STATE_TRANSITION_FAILED",
                        "Cannot transition to CAPTURING after reconfigure");
        if (!state_machine_.transition_to(PipelineState::STOPPING)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to STOPPING after reconfigure failure");
        }
    }
}

/** @brief Release all pipeline resources (frame pump, RTSP clients, encoder, capture backend), in that order. */
void ScreenPipeline::teardown_all() {
    // Clear any saved GPU-texture frame before the D3D11 device is released.
    // do_reconfigure() normally does this, but if stop is requested while in
    // RECONFIGURING, pipeline_thread_func() can reach teardown_all() first;
    // releasing the backend with the texture still held would be a use-after-free.
    initial_frame_buf_  = FrameBuffer{};
    initial_frame_meta_ = {};

    if (frame_pump_) {
        frame_pump_->stop();
        frame_pump_.reset();
    }

    bool was_connected = std::any_of(rtsp_clients_.begin(), rtsp_clients_.end(),
                                     [](const auto& c) { return c->is_connected(); });
    if (was_connected) log_->log_publish_stopped(monitor_info_.number, "teardown");
    for (auto& client : rtsp_clients_) client->disconnect();
    rtsp_clients_.clear();

    if (encoder_) {
        std::vector<EncodedPacket> flush_pkts;
        encoder_->flush(flush_pkts);
        encoder_->reset();
        encoder_.reset();
    }

    if (capture_backend_) {
        capture_backend_->release();
        capture_backend_.reset();
    }
}

/** @brief Pipeline thread body: init, then dispatch on the current state until stop, FATAL or STOPPING. */
void ScreenPipeline::pipeline_thread_func() {
    if (!do_init_and_capture()) return;

    while (!stop_requested_.load()) {
        switch (state_machine_.current_state()) {
            case PipelineState::CAPTURING:
                if (!do_connect()) {
                    // Wait, then retry from CAPTURING
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        config_.rtsp.reconnect_delay_ms));
                }
                break;

            case PipelineState::STREAMING:
                do_streaming_loop();
                break;

            case PipelineState::RECONNECTING:
                do_reconnect();
                break;

            case PipelineState::RECONFIGURING:
                do_reconfigure();
                break;

            case PipelineState::FATAL:
                teardown_all();
                return;

            case PipelineState::STOPPING:
                teardown_all();
                return;

            default:
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                break;
        }
    }

    if (!state_machine_.transition_to(PipelineState::STOPPING)) {
        log_->log_error("STATE_TRANSITION_FAILED",
                        "Cannot transition to STOPPING on shutdown");
    }
    teardown_all();
}

// ════════════════════════════════════════════════════════════
// MonitorSupervisor
// ════════════════════════════════════════════════════════════

MonitorSupervisor::MonitorSupervisor() = default;

MonitorSupervisor::~MonitorSupervisor() {
    request_stop();
    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();
}

bool MonitorSupervisor::init(const AppConfig& config, std::string& error) {
    config_ = config;

    log_ = std::make_shared<LogSink>();
    if (!log_->init(config_.app.log_dir,
                    config_.app.instance_name,
                    config_.app.log_level,
                    error)) {
        return false;
    }

    metrics_ = std::make_shared<MetricsStore>();

    // Audio failure is only logged: video streaming continues without audio.
    if (config_.audio.enabled) {
        auto audio_pipeline = std::make_shared<AudioPipeline>();
        std::string audio_err;
        if (audio_pipeline->start(config_.audio, audio_err, log_)) {
            audio_pipeline_ = std::move(audio_pipeline);
            log_->log_event(spdlog::level::info, "audio_pipeline_started", {});
        } else {
            log_->log_error("AUDIO_INIT_FAILED",
                            "Audio pipeline failed to start, continuing without audio: "
                            + audio_err);
        }
    }

    return true;
}

void MonitorSupervisor::request_stop() {
    stop_requested_.store(true);
}

/**
 * @brief Diff the current monitor list against the previous one and apply the changes.
 *
 * Pipelines are keyed by `stable_id` (stable per physical monitor), so the
 * mapping survives Windows renumbering monitors.
 *
 * Cases handled:
 * - Removed: stable_id absent from the current list -> stop and destroy the pipeline.
 * - Added: stable_id not in the map -> create and start a pipeline.
 * - FATAL: existing pipeline is FATAL -> stop and recreate it.
 * - Retarget: number / is_primary / handle changed (e.g. primary promotion or
 *   renumbering) -> recreate the pipeline so the primary alias and the
 *   Settings-based screen number follow the monitor.
 * - Resolution only: notify_resized() on the existing pipeline.
 */
void MonitorSupervisor::apply_monitor_changes(
        const std::vector<MonitorInfo>& current) {
    std::lock_guard<std::mutex> lk(pipelines_mutex_);

    std::set<std::string> current_ids;
    for (const auto& m : current) current_ids.insert(m.stable_id);

    for (const auto& mi : last_monitors_) {
        if (current_ids.find(mi.stable_id) == current_ids.end()) {
            log_->log_monitor_removed(mi.number);
            auto it = pipelines_.find(mi.stable_id);
            if (it != pipelines_.end()) {
                it->second->stop();
                pipelines_.erase(it);
            }
        }
    }

    for (const auto& mi : current) {
        auto it          = pipelines_.find(mi.stable_id);
        const bool is_new   = (it == pipelines_.end());
        const bool is_fatal = (!is_new && it->second->is_fatal());

        bool needs_retarget = false;
        if (!is_new && !is_fatal) {
            const auto& p = it->second;
            needs_retarget = (p->number()     != mi.number     ||
                              p->is_primary() != mi.is_primary ||
                              p->handle()     != mi.handle);
        }

        if (is_new || is_fatal || needs_retarget) {
            if (is_fatal) {
                it->second->stop();
                pipelines_.erase(it);
                log_->log_event(spdlog::level::warn,
                                "pipeline_restart",
                                {{"monitor",   std::to_string(mi.number)},
                                 {"stable_id", mi.stable_id},
                                 {"reason",    "FATAL pipeline restarting"}});
            } else if (needs_retarget) {
                it->second->stop();
                pipelines_.erase(it);
                log_->log_event(spdlog::level::info,
                                "pipeline_retarget",
                                {{"monitor",   std::to_string(mi.number)},
                                 {"stable_id", mi.stable_id},
                                 {"reason",    "number/primary/handle changed — retargeting pipeline"}});
            } else {
                log_->log_monitor_added(mi.number, mi.device_name,
                                        mi.logical_width, mi.logical_height,
                                        mi.is_primary);
            }
            auto pipeline = std::make_unique<ScreenPipeline>(
                mi, config_, log_, metrics_, audio_pipeline_);
            pipeline->start();
            pipelines_[mi.stable_id] = std::move(pipeline);
        }
    }

    // Notify resolution changes (only for monitors that were not retargeted)
    for (const auto& mi : current) {
        for (const auto& lm : last_monitors_) {
            if (mi.stable_id == lm.stable_id          &&
                mi.number     == lm.number             &&
                mi.is_primary == lm.is_primary         &&
                mi.handle     == lm.handle             &&
                (mi.logical_width  != lm.logical_width ||
                 mi.logical_height != lm.logical_height)) {
                auto it = pipelines_.find(mi.stable_id);
                if (it != pipelines_.end()) {
                    it->second->notify_resized(mi.logical_width, mi.logical_height);
                }
            }
        }
    }

    last_monitors_ = current;
}

/** @brief Poll MonitorDetector every monitor_check_interval_ms and apply the changes. */
void MonitorSupervisor::monitor_poll_loop() {
    while (!stop_requested_.load()) {
        auto monitors = MonitorDetector::enumerate();
        apply_monitor_changes(monitors);
        std::this_thread::sleep_for(std::chrono::milliseconds(
            config_.runtime.monitor_check_interval_ms));
    }
}

/** @brief Write metrics and health files periodically; one final write on stop. */
void MonitorSupervisor::metrics_writer_loop() {
    time_utils::Stopwatch metrics_sw;
    time_utils::Stopwatch health_sw;

    while (!stop_requested_.load()) {
        if (metrics_sw.expired(config_.runtime.emit_metrics_interval_ms)) {
            metrics_->save_metrics(config_.app.metrics_path);
            metrics_sw.reset();
        }
        if (health_sw.expired(config_.runtime.emit_metrics_interval_ms)) {
            metrics_->save_health(config_.app.health_path);
            health_sw.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    metrics_->save_metrics(config_.app.metrics_path);
    metrics_->save_health(config_.app.health_path);
}

/**
 * @brief Start the background threads, block until stop is requested, then shut down gracefully.
 *
 * Monitor changes and FATAL pipeline restarts are handled by monitor_poll_loop().
 */
void MonitorSupervisor::run() {
    metrics_->mark_session_start();

    monitor_thread_ = std::thread(&MonitorSupervisor::monitor_poll_loop,  this);
    metrics_thread_ = std::thread(&MonitorSupervisor::metrics_writer_loop, this);

    while (!stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Stop all pipelines
    {
        std::lock_guard<std::mutex> lk(pipelines_mutex_);
        for (auto& [sid, pipeline] : pipelines_) {
            pipeline->stop();
        }
        pipelines_.clear();
    }

    // Stop audio only after every pipeline has unsubscribed
    if (audio_pipeline_) {
        audio_pipeline_->stop();
    }

    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();

    if (config_.runtime.shutdown_grace_ms > 0)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.runtime.shutdown_grace_ms));

    log_->log_event(spdlog::level::info, "supervisor_stopped", {});
    log_->flush();
}

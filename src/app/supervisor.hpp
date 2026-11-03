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
#include "audio/audio_pipeline.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Capture -> encode -> RTSP pipeline for one monitor.
 *
 * One instance per monitor; its own thread runs the phases according to the
 * internal StateMachine. The primary monitor publishes two RTSP streams
 * (screen{n} and the screen0 alias) from the same encoder.
 */
class ScreenPipeline {
public:
    /**
     * @param info           Target monitor
     * @param config         Application settings (copied)
     * @param log            Log sink
     * @param metrics        Metrics store
     * @param audio_pipeline Shared audio pipeline to subscribe to; nullptr = no audio
     */
    ScreenPipeline(const MonitorInfo& info, const AppConfig& config,
                   std::shared_ptr<LogSink> log,
                   std::shared_ptr<MetricsStore> metrics,
                   std::shared_ptr<AudioPipeline> audio_pipeline = nullptr);

    /** @brief Calls stop(), then unsubscribes from the audio pipeline. */
    ~ScreenPipeline();

    /** @brief Start the pipeline thread. */
    void start();

    /** @brief Request stop and join the pipeline thread (which releases all resources). */
    void stop();

    /**
     * @brief Notify a resolution change; the streaming loop then goes to RECONFIGURING.
     * @param new_width  New logical width (px)
     * @param new_height New logical height (px)
     */
    void notify_resized(int new_width, int new_height);

    PipelineState state() const { return state_machine_.current_state(); }

    /** @brief Monitor number (the Windows Settings display number). */
    int monitor_number() const { return monitor_info_.number; }

    /** @brief Same as monitor_number(). */
    int number() const { return monitor_info_.number; }

    bool is_primary() const { return monitor_info_.is_primary; }

    /** @brief Stable per-physical-monitor ID (CCD monitorDevicePath; device_name if unavailable). */
    const std::string& stable_id() const { return monitor_info_.stable_id; }

    HMONITOR handle() const { return monitor_info_.handle; }

    bool is_fatal() const { return state() == PipelineState::FATAL; }

private:

    void pipeline_thread_func();

    // INIT -> CAPTURING. Returns false (state FATAL) on failure.
    bool do_init_and_capture();

    // CAPTURING -> STREAMING. On RTSP connect failure returns false and stays in
    // CAPTURING (retryable); encoder init failure is FATAL.
    bool do_connect();

    // Runs until stop, an RTSP error, a resolution change or a DXGI hard error.
    void do_streaming_loop();

    // RECONNECTING -> STREAMING, retrying with backoff.
    void do_reconnect();

    // RECONFIGURING -> CAPTURING (re-inits DXGI; the caller then reconnects).
    void do_reconfigure();

    // Releases frame pump, RTSP clients, encoder and capture backend.
    void teardown_all();

    // One URL, or two (screen{n} and screen0) for the primary monitor.
    std::vector<std::string> make_rtsp_urls() const;

    /**
     * @brief Init @p backend with retries (see the definition for the policy).
     * @param backend Backend to init (non-null)
     * @param width   Logical width (px)
     * @param height  Logical height (px)
     * @return true on success
     */
    bool init_capture_with_retry(ICaptureBackend* backend, int width, int height);

    MonitorInfo   monitor_info_;                ///< Target monitor
    AppConfig     config_;                      ///< Application settings
    std::shared_ptr<LogSink>      log_;         ///< Log sink
    std::shared_ptr<MetricsStore> metrics_;     ///< Metrics store

    std::unique_ptr<ICaptureBackend>              capture_backend_;  ///< Capture backend
    std::unique_ptr<FramePump>                    frame_pump_;       ///< Frame pump
    std::unique_ptr<EncoderController>            encoder_;          ///< Encoder
    std::vector<std::unique_ptr<RtspPublisherClient>> rtsp_clients_; ///< One client per RTSP URL

    std::shared_ptr<AudioPipeline>         audio_pipeline_;  ///< Shared audio pipeline (nullptr if audio is off)
    std::shared_ptr<AudioSubscriberQueue>  audio_queue_;     ///< This pipeline's audio subscription (nullptr if audio is off)
    int64_t audio_stream_start_us_ = 0;   ///< Base time for audio pts recomputation (RTCP SR NTP base)
    int64_t last_audio_pts_        = -1;  ///< Last audio pts sent (keeps pts strictly increasing)

    StateMachine  state_machine_;              ///< Pipeline state machine
    std::thread   pipeline_thread_;            ///< Pipeline thread

    std::atomic<bool> stop_requested_{false};  ///< Stop requested
    std::atomic<bool> resize_pending_{false};  ///< Resolution change pending

    struct PendingResize {
        int width  = 0;
        int height = 0;
    };
    mutable std::mutex resize_mutex_;   ///< Guards pending_resize_
    PendingResize      pending_resize_; ///< Next resolution (access under resize_mutex_)

    BackoffState  rtsp_backoff_;               ///< RTSP reconnect backoff
    int           current_width_  = 0;         ///< Current frame width
    int           current_height_ = 0;         ///< Current frame height

    /// Frame used to seed the freeze frame of do_streaming_loop(), so video
    /// flows before DXGI returns its next frame (24 s or more on an idle
    /// monitor). Set by do_init_and_capture()/do_reconfigure() and re-saved when
    /// the streaming loop exits; cleared in do_reconfigure()/teardown_all()
    /// because it may hold a GPU texture of the D3D11 device being released.
    FrameBuffer initial_frame_buf_;
    FrameMeta   initial_frame_meta_{};
};

/**
 * @brief Owns one ScreenPipeline per monitor.
 *
 * Polls for monitor connect/disconnect/resolution changes, creates and
 * destroys pipelines accordingly, and periodically writes metrics and health files.
 */
class MonitorSupervisor {
public:
    MonitorSupervisor();

    /** @brief Requests stop and joins the background threads. */
    ~MonitorSupervisor();

    /**
     * @brief Init logging, metrics and (if enabled) the shared audio pipeline.
     *
     * An audio pipeline start failure is only logged; video streams without audio.
     * @param config  Application settings
     * @param[out] error Message on failure (log sink init)
     * @return true on success
     */
    bool init(const AppConfig& config, std::string& error);

    /** @brief Run until request_stop() is called, then shut everything down. */
    void run();

    /** @brief Make run() return. Thread-safe (called from the console handler). */
    void request_stop();

private:

    void monitor_poll_loop();

    void metrics_writer_loop();

    // Diffs against last_monitors_ and starts/stops/notifies pipelines (see the definition).
    void apply_monitor_changes(const std::vector<MonitorInfo>& current);

    AppConfig config_;                          ///< Application settings
    std::shared_ptr<LogSink>      log_;         ///< Log sink
    std::shared_ptr<MetricsStore> metrics_;     ///< Metrics store
    std::shared_ptr<AudioPipeline> audio_pipeline_;  ///< Shared audio pipeline; nullptr if audio is disabled or start() failed

    std::map<std::string, std::unique_ptr<ScreenPipeline>> pipelines_;  ///< stable_id -> pipeline
    std::mutex pipelines_mutex_;                                         ///< Guards pipelines_ and last_monitors_

    std::vector<MonitorInfo> last_monitors_;    ///< Monitor list from the previous poll

    std::thread monitor_thread_;               ///< Monitor polling thread (monitor_poll_loop)
    std::thread metrics_thread_;               ///< Metrics writer thread (metrics_writer_loop)
    std::atomic<bool> stop_requested_{false};  ///< Stop requested
};

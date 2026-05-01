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
                                std::shared_ptr<MetricsStore> metrics)
    : monitor_info_(info)
    , config_(config)
    , log_(std::move(log))
    , metrics_(std::move(metrics)) {
    // ステート遷移コールバックを登録する
    state_machine_.on_transition([this](PipelineState from, PipelineState to) {
        log_->log_state_changed(monitor_info_.number,
                                state_name(from), state_name(to));
        metrics_->set_state(monitor_info_.number, state_name(to));
    });
}

ScreenPipeline::~ScreenPipeline() {
    stop();
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
 * @brief RTSP URL を生成する
 *
 * path_pattern 内の {n} をモニター番号に置換する。
 * プライマリモニターの場合は screen0 エイリアスも追加する。
 */
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
        urls.push_back(make_url(0));  // screen0 エイリアス
    return urls;
}

/**
 * @brief DXGI キャプチャと FramePump を初期化する
 *
 * 失敗した場合は FATAL ステートに遷移して false を返す。
 */
bool ScreenPipeline::do_init_and_capture() {
    // DXGI キャプチャバックエンドを初期化する
    auto backend = std::make_unique<DxgiCaptureBackend>();
    if (!backend->init(monitor_info_.handle,
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

    // FramePump を開始する
    frame_pump_ = std::make_unique<FramePump>();
    frame_pump_->start(capture_backend_.get(), config_.capture.frame_timeout_ms);

    if (!state_machine_.transition_to(PipelineState::CAPTURING)) {
        log_->log_error("STATE_TRANSITION_FAILED", "Cannot transition to CAPTURING");
        frame_pump_->stop();
        capture_backend_->release();
        return false;
    }

    // 最初のフレームを待って解像度を確認する。
    // DXGI は画面変化がなくても DuplicateOutput 直後の最初のフレームは返すが、
    // 高負荷時や静止画面では時間がかかる場合があるため十分な時間を与える。
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

    return true;
}

/**
 * @brief エンコーダーと RTSP クライアントを初期化して接続する
 *
 * エンコーダー初期化失敗は FATAL、RTSP 接続失敗は CAPTURING に戻す。
 */
bool ScreenPipeline::do_connect() {
    // CAPTURING → CONNECTING に遷移する
    if (!state_machine_.transition_to(PipelineState::CONNECTING)) {
        log_->log_error("STATE_TRANSITION_FAILED", "Cannot transition to CONNECTING");
        return false;
    }

    // エンコーダーを初期化する
    encoder_ = std::make_unique<EncoderController>();
    std::string enc_err;
    if (!encoder_->init(config_.encoder,
                        static_cast<uint32_t>(current_width_),
                        static_cast<uint32_t>(current_height_),
                        enc_err)) {
        log_->log_error("ENCODER_INIT_FAILED", enc_err);
        encoder_.reset();
        if (!state_machine_.transition_to(PipelineState::FATAL)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to FATAL after encoder init failure");
        }
        return false;
    }

    const std::string& actual_codec = encoder_->selected_codec_name();
    log_->log_encoder_initialized(monitor_info_.number,
                                   actual_codec,
                                   current_width_, current_height_,
                                   config_.encoder.fps,
                                   config_.encoder.bitrate_kbps);
    metrics_->set_encoder_codec(monitor_info_.number, actual_codec);

    // RTSP クライアントを接続する
    auto codec_info = encoder_->get_codec_info();
    auto urls = make_rtsp_urls();
    rtsp_clients_.clear();

    for (const auto& url : urls) {
        auto client = std::make_unique<RtspPublisherClient>();
        std::string rtsp_err;
        if (!client->connect(url, config_.rtsp, codec_info, rtsp_err)) {
            log_->log_error("RTSP_CONNECT_FAILED", rtsp_err,
                            {{"url", url},
                             {"monitor", std::to_string(monitor_info_.number)}});
            rtsp_clients_.clear();
            encoder_.reset();
            // RTSP 接続失敗は CAPTURING に戻してリトライ可能にする
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

    // STREAMING に遷移する
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
 * @brief エンコード・送信ループを実行する
 *
 * STREAMING ステート中に呼び出す。フレームを取得してエンコードし、
 * 全 RTSP クライアントに送信する。
 *
 * DXGI Desktop Duplication は画面に変化がない場合フレームを送らないため、
 * 最後に受信したフレームを設定 fps に合わせて繰り返し送信する（フリーズフレーム）。
 * DXGI ハードエラー（ACCESS_LOST 等）を検出した場合は即座に RECONFIGURING に遷移する。
 */
void ScreenPipeline::do_streaming_loop() {
    time_utils::Stopwatch metrics_sw;
    uint64_t bytes_since_last  = 0;
    uint64_t frames_since_last = 0;

    // 静止画面でのフリーズフレーム繰り返し用キャッシュ
    FrameBuffer freeze_buf;
    FrameMeta   freeze_meta{};
    bool        has_freeze = false;

    // 設定 fps に基づくフレーム送信間隔 (μs)
    const int64_t frame_interval_us = 1000000LL / config_.encoder.fps;
    time_utils::Stopwatch frame_sw;  // フレーム送信タイマー

    while (!stop_requested_.load() &&
           state_machine_.current_state() == PipelineState::STREAMING) {

        // DXGI ハードエラー（ACCESS_LOST 等）を検出したら即座に再初期化する
        // タイムアウト（画面変化なし）とは区別し、静止画面では RECONFIGURING に遷移しない
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

        // 次のフレーム送信時刻までの残り待機時間を計算する
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
            // 新しいフレームを受信: フリーズバッファを更新する
            freeze_buf  = std::move(buf);
            freeze_meta = meta;
            has_freeze  = true;
            metrics_->increment_frames_received(monitor_info_.number);
        }

        // 送信間隔に達していなければスキップする（新規フレームも同様）
        // これにより画面更新が速い場合でも設定 fps を超えて送信しない
        if (frame_sw.elapsed_us() < frame_interval_us) {
            continue;
        }

        // フリーズフレームがなければ最初のフレームがまだ来ていない
        if (!has_freeze) {
            continue;
        }

        // 送信タイムスタンプを現在時刻に統一する（フリーズフレーム・新規フレーム共通）
        freeze_meta.timestamp_us = time_utils::system_now_us();

        // フレーム送信タイマーをリセットする
        frame_sw.reset();

        // 解像度変更通知が来ていれば RECONFIGURING に遷移する
        if (resize_pending_.load()) {
            if (!state_machine_.transition_to(PipelineState::RECONFIGURING)) {
                log_->log_error("STATE_TRANSITION_FAILED",
                                "Cannot transition to RECONFIGURING");
            }
            break;
        }

        // エンコードする
        std::vector<EncodedPacket> packets;
        if (!encoder_->encode(freeze_buf, freeze_meta, packets)) {
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

        // 全 RTSP クライアントにパケットを送信する
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

            // 1 秒ごとに FPS とビットレートのメトリクスを更新する
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
}

/**
 * @brief RTSP 再接続をバックオフ付きで実行する
 *
 * 再接続に成功した場合は STREAMING に遷移する。
 * stop_requested_ が立った場合は STOPPING に遷移して終了する。
 */
void ScreenPipeline::do_reconnect() {
    // 接続中のクライアントがあれば停止を1回だけ記録して解放する
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

        // 全 URL に再接続を試みる
        auto urls = make_rtsp_urls();
        auto codec_info = encoder_->get_codec_info();
        // 再接続時は最初のフレームの時刻を RTCP SR の NTP 基点として渡す。
        // これにより接続時の av_gettime() ではなく実際のストリーム開始時刻が使われ、
        // VLC が受信するパケットのタイムスタンプが現在時刻より大幅にずれるのを防ぐ。
        codec_info.stream_start_us = encoder_->first_frame_time_us();
        bool all_ok = true;
        std::vector<std::unique_ptr<RtspPublisherClient>> new_clients;

        for (const auto& url : urls) {
            auto client = std::make_unique<RtspPublisherClient>();
            std::string err;
            if (!client->connect(url, config_.rtsp, codec_info, err)) {
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
            return;
        }
    }

    // 停止リクエストが来た場合
    if (!state_machine_.transition_to(PipelineState::STOPPING)) {
        log_->log_error("STATE_TRANSITION_FAILED",
                        "Cannot transition to STOPPING from RECONNECTING");
    }
}

/**
 * @brief 解像度変更に対応して再初期化する
 *
 * FramePump を停止し、DXGI バックエンドを再構成する。
 * エンコーダーと RTSP クライアントを解放して CAPTURING に遷移し、
 * pipeline_thread_func の CAPTURING ケースで再接続させる。
 */
void ScreenPipeline::do_reconfigure() {
    // フレームポンプを一時停止する
    if (frame_pump_) frame_pump_->stop();

    // 接続中のクライアントがあれば停止を1回だけ記録して解放する
    bool was_connected = std::any_of(rtsp_clients_.begin(), rtsp_clients_.end(),
                                     [](const auto& c) { return c->is_connected(); });
    if (was_connected) log_->log_publish_stopped(monitor_info_.number, "reconfiguring");
    for (auto& client : rtsp_clients_) client->disconnect();
    rtsp_clients_.clear();

    // エンコーダーをフラッシュして解放する
    if (encoder_) {
        std::vector<EncodedPacket> flush_pkts;
        encoder_->flush(flush_pkts);
        encoder_->reset();
        encoder_.reset();
    }

    // 解像度変更フラグをアトミックに取得してクリアし、保護下でサイズを読み取る
    bool had_resize = resize_pending_.exchange(false);
    PendingResize pending;
    if (had_resize) {
        std::lock_guard<std::mutex> lk(resize_mutex_);
        pending = pending_resize_;
    }
    int new_w = had_resize ? pending.width  : current_width_;
    int new_h = had_resize ? pending.height : current_height_;

    // DXGI バックエンドを完全に再初期化する (ACCESS_LOST 後の回復を含む)
    if (capture_backend_) {
        capture_backend_->release();
        if (!capture_backend_->init(monitor_info_.handle, new_w, new_h)) {
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

    // FramePump を再開する
    if (frame_pump_ && capture_backend_) {
        frame_pump_->start(capture_backend_.get(), config_.capture.frame_timeout_ms);
    }

    // CAPTURING に戻す。次のループで do_connect() が呼ばれる
    if (!state_machine_.transition_to(PipelineState::CAPTURING)) {
        log_->log_error("STATE_TRANSITION_FAILED",
                        "Cannot transition to CAPTURING after reconfigure");
        if (!state_machine_.transition_to(PipelineState::STOPPING)) {
            log_->log_error("STATE_TRANSITION_FAILED",
                            "Cannot transition to STOPPING after reconfigure failure");
        }
    }
}

/**
 * @brief すべてのパイプラインリソースを解放する
 */
void ScreenPipeline::teardown_all() {
    if (frame_pump_) {
        frame_pump_->stop();
        frame_pump_.reset();
    }

    // 接続中のクライアントがあれば停止を1回だけ記録して解放する
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

/**
 * @brief パイプラインスレッドのメイン関数
 *
 * INIT からスタートし、ステートマシンに従って各フェーズを実行する。
 */
void ScreenPipeline::pipeline_thread_func() {
    // INIT → CAPTURING へ初期化する
    if (!do_init_and_capture()) return;

    while (!stop_requested_.load()) {
        switch (state_machine_.current_state()) {
            case PipelineState::CAPTURING:
                // エンコーダーと RTSP クライアントを接続する
                if (!do_connect()) {
                    // RTSP 接続失敗。少し待ってから CAPTURING ステートで再試行する
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

    // ログシンクを初期化する
    log_ = std::make_shared<LogSink>();
    if (!log_->init(config_.app.log_dir,
                    config_.app.instance_name,
                    config_.app.log_level,
                    error)) {
        return false;
    }

    metrics_ = std::make_shared<MetricsStore>();

    return true;
}

void MonitorSupervisor::request_stop() {
    stop_requested_.store(true);
}

/**
 * @brief 現在のモニター一覧と前回の一覧を比較して差分を適用する
 */
void MonitorSupervisor::apply_monitor_changes(
        const std::vector<MonitorInfo>& current) {
    std::lock_guard<std::mutex> lk(pipelines_mutex_);

    // モニター番号のセットを構築する
    std::set<int> current_nums, last_nums;
    for (const auto& m : current)       current_nums.insert(m.number);
    for (const auto& m : last_monitors_) last_nums.insert(m.number);

    // 削除されたモニターのパイプラインを停止する
    for (const auto& mi : last_monitors_) {
        if (current_nums.find(mi.number) == current_nums.end()) {
            log_->log_monitor_removed(mi.number);
            auto it = pipelines_.find(mi.number);
            if (it != pipelines_.end()) {
                it->second->stop();
                pipelines_.erase(it);
            }
        }
    }

    // 追加されたモニター、または FATAL 状態になったパイプラインを(再)起動する
    for (const auto& mi : current) {
        auto it = pipelines_.find(mi.number);
        const bool is_new   = (it == pipelines_.end());
        const bool is_fatal = (!is_new && it->second->is_fatal());

        if (is_new || is_fatal) {
            if (is_fatal) {
                // FATAL パイプラインを停止して削除し、再生成する
                it->second->stop();
                pipelines_.erase(it);
                log_->log_event(spdlog::level::warn,
                                "pipeline_restart",
                                {{"monitor", std::to_string(mi.number)},
                                 {"reason",  "FATAL pipeline restarting"}});
            } else {
                log_->log_monitor_added(mi.number, mi.device_name,
                                        mi.logical_width, mi.logical_height,
                                        mi.is_primary);
            }
            auto pipeline = std::make_unique<ScreenPipeline>(
                mi, config_, log_, metrics_);
            pipeline->start();
            pipelines_[mi.number] = std::move(pipeline);
        }
    }

    // 解像度が変更されたモニターに通知する
    for (const auto& mi : current) {
        for (const auto& lm : last_monitors_) {
            if (mi.number == lm.number &&
                (mi.logical_width  != lm.logical_width ||
                 mi.logical_height != lm.logical_height)) {
                auto it = pipelines_.find(mi.number);
                if (it != pipelines_.end()) {
                    it->second->notify_resized(mi.logical_width, mi.logical_height);
                }
            }
        }
    }

    last_monitors_ = current;
}

/**
 * @brief モニター変化ポーリングスレッドのメイン関数
 */
void MonitorSupervisor::monitor_poll_loop() {
    while (!stop_requested_.load()) {
        auto monitors = MonitorDetector::enumerate();
        apply_monitor_changes(monitors);
        std::this_thread::sleep_for(std::chrono::milliseconds(
            config_.runtime.monitor_check_interval_ms));
    }
}

/**
 * @brief メトリクス書き込みスレッドのメイン関数
 */
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

    // 最終フラッシュ
    metrics_->save_metrics(config_.app.metrics_path);
    metrics_->save_health(config_.app.health_path);
}

/**
 * @brief メインループを実行する
 *
 * バックグラウンドスレッドを開始し、FATAL パイプラインの監視と
 * グレースフルシャットダウンを担当する。
 */
void MonitorSupervisor::run() {
    metrics_->mark_session_start();

    // バックグラウンドスレッドを開始する
    monitor_thread_ = std::thread(&MonitorSupervisor::monitor_poll_loop,  this);
    metrics_thread_ = std::thread(&MonitorSupervisor::metrics_writer_loop, this);

    // メインループ: 停止リクエストを待つ
    // FATAL パイプラインの再起動は monitor_poll_loop → apply_monitor_changes で処理する
    while (!stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // グレースフルシャットダウン: 全パイプラインを停止する
    {
        std::lock_guard<std::mutex> lk(pipelines_mutex_);
        for (auto& [num, pipeline] : pipelines_) {
            pipeline->stop();
        }
        pipelines_.clear();
    }

    if (monitor_thread_.joinable()) monitor_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();

    if (config_.runtime.shutdown_grace_ms > 0)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.runtime.shutdown_grace_ms));

    log_->log_event(spdlog::level::info, "supervisor_stopped", {});
    log_->flush();
}

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
 * @brief DXGI キャプチャバックエンドをリトライ付きで初期化する
 *
 * do_init_and_capture() / do_reconfigure() で共用する共通ヘルパー。
 * 200ms 間隔で最大 15 回リトライし、stop_requested_ が立てば途中で打ち切る。
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
    // DXGI キャプチャバックエンドを初期化する。
    // 解像度変更直後や GPU を共有する別モニターの RECONFIGURING と競合した場合、
    // DuplicateOutput が E_ACCESSDENIED を返すことがある。
    // do_reconfigure() と同様に 200ms 間隔で最大 15 回リトライする。
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

    // 初期フリーズフレームとして保存する。
    // DXGI Desktop Duplication は画面に変化がない限り次のフレームを返さないため、
    // do_streaming_loop() の freeze_buf がアイドルモニターで長時間空のままになる
    // （結果としてストリームが真っ暗になる）問題を防ぐ。
    // このフレームは GPU ゼロコピーモード切り替え前（CPU パス）で取得されるため
    // gpu_texture は null だが、encode_gpu_zero_copy() が UpdateSubresource で
    // 対応するため問題ない。
    initial_frame_buf_  = std::move(buf);   // buf は以降不要のため move して不要なコピーを避ける
    initial_frame_meta_ = meta;

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
    // GPU ゼロコピーパスを有効化できるか試すため、キャプチャバックエンドが
    // 物理解像度=論理解像度（スケーリング不要）の場合に限り D3D11 デバイスを共有する
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

    // エンコーダーが GPU ゼロコピーパスを確立できた場合のみキャプチャ側を
    // ゼロコピーモードに切り替える（CPU 側の色空間変換・読み戻しを完全に省略する）
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

    // 静止画面でのフリーズフレーム繰り返し用キャッシュ。
    // do_init_and_capture() が保存した初期フレームを起点とすることで、
    // DXGI が次のフレームを返すまでの間（アイドルモニターでは 24 秒以上）
    // ストリームが真っ暗になる問題を防ぐ。
    FrameBuffer freeze_buf  = std::move(initial_frame_buf_);
    FrameMeta   freeze_meta = initial_frame_meta_;
    bool        has_freeze  = (!freeze_buf.data.empty() || freeze_buf.gpu_texture != nullptr);
    // 使用したら解放してメモリを節約する（高解像度フレームは数十 MB になる）
    initial_frame_meta_ = {};

    // 直前に encode() したフレームからピクセル内容が変化しているか。
    // DXGI Desktop Duplication は画面に変化があった場合のみ新規フレームを
    // 返すため、新規フレームを受信した時点で true にする。フリーズフレーム
    // （新規フレームなし）の再送時は内容が同一なので false のまま encode() に
    // 渡し、sws_scale による色空間変換コストを省略させる。
    bool content_changed = true;

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
            freeze_buf       = std::move(buf);
            freeze_meta      = meta;
            has_freeze       = true;
            content_changed  = true;
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

        // エンコードする。content_changed が false の場合、エンコーダー側で
        // sws_scale をスキップして直前の変換結果を再利用する（CPU 負荷削減）
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
        // 変換済み YUV フレームは消費されたので、次に新規フレームを受信するまでは
        // 内容が変化していない（再変換不要）
        content_changed = false;

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

    // RECONNECT (STREAMING → RECONNECTING → STREAMING) 後に再び do_streaming_loop() が
    // 呼ばれると initial_frame_buf_ は空（最初の呼び出しで std::move 済み）のため、
    // GPU ゼロコピーモード中に静止画面のモニターでは DXGI が新規フレームを
    // 返さず has_freeze = false のままストリームが真っ暗になる。
    // ループ終了時に最後のフリーズフレームを保存しておくことで次回呼び出し時に
    // 即座に送信できる状態にする。
    // ※ GPU テクスチャを含む場合は do_reconfigure() でクリアすること
    //    （D3D11 デバイス解放後に古いテクスチャを参照し続けるのを防ぐため）。
    if (has_freeze) {
        initial_frame_buf_  = std::move(freeze_buf);
        initial_frame_meta_ = freeze_meta;
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
            // 再接続直後の最初のフレームを IDR として送出する。
            // エンコーダーをリセットせず GOP 内に挿入するため PTS の連続性は保たれる。
            // これにより接続直後から受信側が黒画面なく映像を表示できる。
            encoder_->request_next_idr();
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

    // D3D11 デバイス解放前に GPU テクスチャ参照を含む初期フレームバッファを
    // クリアする。do_streaming_loop() 終了時に保存された freeze_buf が
    // GPU テクスチャを持つ場合、ここでクリアしないと capture_backend_->release()
    // 後も古いデバイスのテクスチャが参照され続けることになる。
    initial_frame_buf_  = FrameBuffer{};
    initial_frame_meta_ = {};

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
    //
    // 解像度変更直後は Windows のディスプレイドライバーが構成を更新中であり、
    // DuplicateOutput が E_ACCESSDENIED (0x80070005) を返すことがある。
    // 同じ GPU を共有する全モニターが同時に RECONFIGURING に入る場合も同様。
    // 致命的エラーと判断する前に init_capture_with_retry() で最大 15 回リトライする
    // (最大待機 3s。通常 1～2 回目のリトライで成功する)。
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

    // FramePump を再開して最初のフレームを待つ。
    // DXGI は新規の DuplicateOutput 直後の最初の AcquireNextFrame で
    // 必ず現在のデスクトップフレームを返す（画面変化がなくても）。
    // これを初期フリーズフレームとして保存することで do_streaming_loop() の
    // 起動直後から映像が届くようにする（アイドルモニターで最大 24 秒かかる
    // GPU ゼロコピーモード下での次フレーム到着を待たずに済む）。
    if (frame_pump_ && capture_backend_) {
        frame_pump_->start(capture_backend_.get(), config_.capture.frame_timeout_ms);

        FrameBuffer seed_fb;
        FrameMeta   seed_meta;
        if (frame_pump_->wait_pop(seed_fb, seed_meta, config_.capture.frame_timeout_ms)) {
            initial_frame_buf_  = std::move(seed_fb);
            initial_frame_meta_ = seed_meta;
        }
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
    // GPU テクスチャを含む初期フレームバッファを D3D11 デバイス解放前にクリアする。
    // 通常フローでは do_reconfigure() がクリア済みだが、stop_requested_ が
    // RECONFIGURING ステート中に立った場合、do_reconfigure() の実行前に
    // pipeline_thread_func が停止条件を満たして teardown_all() を呼ぶことがある。
    // そのまま capture_backend_->release() を呼ぶと、古い D3D11 デバイスへの
    // GPU テクスチャ参照が残り use-after-free になるため、ここで確実にクリアする。
    initial_frame_buf_  = FrameBuffer{};
    initial_frame_meta_ = {};

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
 *
 * パイプライン管理には物理モニター単位で安定な `stable_id` をキーとして使用する。
 * これにより、Windows がモニター番号を再パックしてもパイプラインと物理モニターの
 * 対応が崩れない。
 *
 * 以下の 4 ケースを処理する:
 * - **削除**: stable_id が現行に無ければパイプラインを停止・破棄する。
 * - **追加**: 現行の stable_id がマップに無ければ新規パイプラインを生成・起動する。
 * - **FATAL 再起動**: 既存パイプラインが FATAL ならば停止・再生成する。
 * - **再ターゲット**: number / is_primary / handle が変化した場合（プライマリ昇格や
 *   番号再割り当てなど）、パイプラインを stop → 再生成することで /screen0 の
 *   プライマリ追従と screenN の Settings 番号追従を保証する。
 * - **解像度のみ変化**: 上記と独立に notify_resized() で軽量対処する。
 */
void MonitorSupervisor::apply_monitor_changes(
        const std::vector<MonitorInfo>& current) {
    std::lock_guard<std::mutex> lk(pipelines_mutex_);

    // stable_id のセットを構築する
    std::set<std::string> current_ids;
    for (const auto& m : current) current_ids.insert(m.stable_id);

    // 削除されたモニターのパイプラインを停止する
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

    // 追加・FATAL・再ターゲットのパイプラインを処理する
    for (const auto& mi : current) {
        auto it          = pipelines_.find(mi.stable_id);
        const bool is_new   = (it == pipelines_.end());
        const bool is_fatal = (!is_new && it->second->is_fatal());

        // 再ターゲット判定: 同一物理モニターで number / is_primary / handle が変化した場合
        // → /screen0 のプライマリ追従・screenN の Settings 番号追従のためにパイプラインを再生成する
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
                mi, config_, log_, metrics_);
            pipeline->start();
            pipelines_[mi.stable_id] = std::move(pipeline);
        }
    }

    // 解像度が変更されたモニターに通知する（再ターゲット不要なもののみ）
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
        for (auto& [sid, pipeline] : pipelines_) {
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

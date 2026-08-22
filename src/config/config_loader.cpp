#include "config/config_loader.hpp"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief encoder セクションを JSON からパースする
 * @param j   JSON オブジェクト
 * @param cfg 出力先のエンコーダー設定
 */
static void parse_encoder(const json& j, EncoderConfig& cfg) {
    if (j.contains("codec"))           cfg.codec           = j["codec"].get<std::string>();
    if (j.contains("fallback_codecs")) {
        // fallback_codecs は文字列の配列
        cfg.fallback_codecs.clear();
        for (const auto& item : j["fallback_codecs"]) {
            cfg.fallback_codecs.push_back(item.get<std::string>());
        }
    }
    if (j.contains("bitrate_kbps"))    cfg.bitrate_kbps    = j["bitrate_kbps"].get<int>();
    if (j.contains("fps"))             cfg.fps             = j["fps"].get<int>();
    if (j.contains("gop_size"))        cfg.gop_size        = j["gop_size"].get<int>();
    if (j.contains("max_b_frames"))    cfg.max_b_frames    = j["max_b_frames"].get<int>();
    if (j.contains("preset"))          cfg.preset          = j["preset"].get<std::string>();
    if (j.contains("tune"))            cfg.tune            = j["tune"].get<std::string>();
    if (j.contains("threads"))         cfg.threads         = j["threads"].get<int>();
}

/**
 * @brief audio セクションを JSON からパースする
 * @param j   JSON オブジェクト
 * @param cfg 出力先の音声設定
 */
static void parse_audio(const json& j, AudioConfig& cfg) {
    if (j.contains("enabled"))       cfg.enabled       = j["enabled"].get<bool>();
    if (j.contains("source"))        cfg.source        = j["source"].get<std::string>();
    if (j.contains("device_id"))     cfg.device_id      = j["device_id"].get<std::string>();
    if (j.contains("bitrate_kbps"))  cfg.bitrate_kbps   = j["bitrate_kbps"].get<int>();
    if (j.contains("sample_rate"))   cfg.sample_rate    = j["sample_rate"].get<int>();
    if (j.contains("channels"))      cfg.channels       = j["channels"].get<int>();
}

/**
 * @brief rtsp セクションを JSON からパースする
 * @param j   JSON オブジェクト
 * @param cfg 出力先の RTSP 設定
 */
static void parse_rtsp(const json& j, RtspConfig& cfg) {
    if (j.contains("base_url"))                    cfg.base_url                    = j["base_url"].get<std::string>();
    if (j.contains("path_pattern"))                cfg.path_pattern                = j["path_pattern"].get<std::string>();
    if (j.contains("connect_timeout_ms"))          cfg.connect_timeout_ms          = j["connect_timeout_ms"].get<int>();
    if (j.contains("send_timeout_ms"))             cfg.send_timeout_ms             = j["send_timeout_ms"].get<int>();
    if (j.contains("reconnect_delay_ms"))          cfg.reconnect_delay_ms          = j["reconnect_delay_ms"].get<int>();
    if (j.contains("reconnect_max_delay_ms"))      cfg.reconnect_max_delay_ms      = j["reconnect_max_delay_ms"].get<int>();
    if (j.contains("reconnect_backoff_multiplier"))
        cfg.reconnect_backoff_multiplier = j["reconnect_backoff_multiplier"].get<float>();
}

bool ConfigLoader::load(const std::string& path, AppConfig& out, std::string& error) {
    // ファイルを開く
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "Cannot open config file: " + path;
        return false;
    }

    // JSON パース
    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    try {
        // app セクション
        if (j.contains("app")) {
            const auto& a = j["app"];
            if (a.contains("instance_name")) out.app.instance_name = a["instance_name"].get<std::string>();
            if (a.contains("log_dir"))        out.app.log_dir        = a["log_dir"].get<std::string>();
            if (a.contains("log_level"))      out.app.log_level      = a["log_level"].get<std::string>();
            if (a.contains("metrics_path"))   out.app.metrics_path   = a["metrics_path"].get<std::string>();
            if (a.contains("health_path"))    out.app.health_path    = a["health_path"].get<std::string>();
        }

        // capture セクション
        if (j.contains("capture")) {
            const auto& c = j["capture"];
            if (c.contains("backend"))          out.capture.backend          = c["backend"].get<std::string>();
            if (c.contains("frame_timeout_ms")) out.capture.frame_timeout_ms = c["frame_timeout_ms"].get<int>();
        }

        // encoder セクション
        if (j.contains("encoder")) parse_encoder(j["encoder"], out.encoder);

        // rtsp セクション
        if (j.contains("rtsp")) parse_rtsp(j["rtsp"], out.rtsp);

        // audio セクション（省略時は無効のまま）
        if (j.contains("audio")) parse_audio(j["audio"], out.audio);

        // runtime セクション
        if (j.contains("runtime")) {
            const auto& r = j["runtime"];
            if (r.contains("monitor_check_interval_ms")) out.runtime.monitor_check_interval_ms = r["monitor_check_interval_ms"].get<int>();
            if (r.contains("shutdown_grace_ms"))         out.runtime.shutdown_grace_ms         = r["shutdown_grace_ms"].get<int>();
            if (r.contains("emit_metrics_interval_ms"))  out.runtime.emit_metrics_interval_ms  = r["emit_metrics_interval_ms"].get<int>();
        }

    } catch (const json::type_error& e) {
        error = std::string("Config type error (フィールドの型が正しくありません): ") + e.what();
        return false;
    } catch (const json::exception& e) {
        error = std::string("Config read error: ") + e.what();
        return false;
    }

    // --- バリデーション ---

    // RTSP base_url が "rtsp://" で始まることを確認
    if (out.rtsp.base_url.empty() || out.rtsp.base_url.rfind("rtsp://", 0) != 0) {
        error = "rtsp.base_url must start with rtsp://";
        return false;
    }

    // エンコーダー FPS の範囲チェック
    if (out.encoder.fps <= 0 || out.encoder.fps > 240) {
        error = "encoder.fps must be between 1 and 240";
        return false;
    }

    // エンコーダービットレートの正値チェック
    if (out.encoder.bitrate_kbps <= 0) {
        error = "encoder.bitrate_kbps must be positive";
        return false;
    }

    // キャプチャバックエンドのチェック (現時点では "dxgi" のみ実装)
    if (out.capture.backend != "dxgi") {
        error = "capture.backend must be \"dxgi\" (only dxgi is currently supported)";
        return false;
    }

    // キャプチャタイムアウトの非負チェック (0 は即時返却)
    if (out.capture.frame_timeout_ms < 0) {
        error = "capture.frame_timeout_ms must be 0 or positive";
        return false;
    }

    // RTSP タイムアウトの正値チェック
    if (out.rtsp.connect_timeout_ms <= 0) {
        error = "rtsp.connect_timeout_ms must be positive";
        return false;
    }
    if (out.rtsp.send_timeout_ms <= 0) {
        error = "rtsp.send_timeout_ms must be positive";
        return false;
    }

    // 再接続待機時間のチェック
    if (out.rtsp.reconnect_delay_ms <= 0) {
        error = "rtsp.reconnect_delay_ms must be positive";
        return false;
    }
    if (out.rtsp.reconnect_max_delay_ms <= 0) {
        error = "rtsp.reconnect_max_delay_ms must be positive";
        return false;
    }
    if (out.rtsp.reconnect_max_delay_ms < out.rtsp.reconnect_delay_ms) {
        error = "rtsp.reconnect_max_delay_ms must be >= rtsp.reconnect_delay_ms";
        return false;
    }
    if (out.rtsp.reconnect_backoff_multiplier < 1.0f) {
        error = "rtsp.reconnect_backoff_multiplier must be >= 1.0";
        return false;
    }

    // 音声設定のチェック（無効時はスキップ）
    if (out.audio.enabled) {
        AudioSourceKind source_kind;
        if (!parse_audio_source_kind(out.audio.source, source_kind)) {
            error = "audio.source must be \"loopback\" or \"capture\"";
            return false;
        }
        if (out.audio.bitrate_kbps <= 0) {
            error = "audio.bitrate_kbps must be positive";
            return false;
        }
        if (out.audio.sample_rate <= 0) {
            error = "audio.sample_rate must be positive";
            return false;
        }
        if (out.audio.channels != 1 && out.audio.channels != 2) {
            error = "audio.channels must be 1 or 2";
            return false;
        }
    }

    // ランタイム間隔の正値チェック
    if (out.runtime.monitor_check_interval_ms <= 0) {
        error = "runtime.monitor_check_interval_ms must be positive";
        return false;
    }
    if (out.runtime.shutdown_grace_ms < 0) {
        error = "runtime.shutdown_grace_ms must be positive";
        return false;
    }
    if (out.runtime.emit_metrics_interval_ms <= 0) {
        error = "runtime.emit_metrics_interval_ms must be positive";
        return false;
    }

    return true;
}

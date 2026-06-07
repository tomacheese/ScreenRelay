#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>

/**
 * @brief モニター単位のパイプラインステート
 *
 * 各モニターのキャプチャ・配信パイプラインが取りうる状態を表す。
 */
enum class PipelineState {
    INIT,           ///< 初期化前
    CAPTURING,      ///< キャプチャ中 (RTSP 接続前)
    CONNECTING,     ///< RTSP 接続中
    STREAMING,      ///< 正常配信中
    RECONNECTING,   ///< RTSP 再接続中
    RECONFIGURING,  ///< 解像度変更対応中
    STOPPING,       ///< グレースフルシャットダウン中
    FATAL           ///< 回復不能エラー
};

/**
 * @brief PipelineState を文字列に変換する
 * @param s 変換対象のステート
 * @return ステート名の文字列 (定数)
 */
inline const char* state_name(PipelineState s) {
    switch (s) {
        case PipelineState::INIT:          return "INIT";
        case PipelineState::CAPTURING:     return "CAPTURING";
        case PipelineState::CONNECTING:    return "CONNECTING";
        case PipelineState::STREAMING:     return "STREAMING";
        case PipelineState::RECONNECTING:  return "RECONNECTING";
        case PipelineState::RECONFIGURING: return "RECONFIGURING";
        case PipelineState::STOPPING:      return "STOPPING";
        case PipelineState::FATAL:         return "FATAL";
        default:                           return "UNKNOWN";
    }
}

/**
 * @brief ピクセルフォーマット
 */
enum class PixelFormat { BGRA, RGBA, RGB, BGR };

/**
 * @brief フレームバッファ
 *
 * キャプチャしたピクセルデータを保持する構造体。
 */
struct FrameBuffer {
    std::vector<uint8_t> data;  ///< ピクセルデータ
    uint32_t width  = 0;         ///< 幅 (ピクセル)
    uint32_t height = 0;         ///< 高さ (ピクセル)
    PixelFormat format = PixelFormat::BGRA;  ///< ピクセルフォーマット

    /**
     * @brief GPU ゼロコピーパス用の D3D11 テクスチャハンドル (型消去)
     *
     * 実体は ID3D11Texture2D* への参照カウント付きハンドルであり、
     * カスタムデリーターが Release() を呼び出す。共通ヘッダーを D3D11 に
     * 依存させないため shared_ptr<void> で型消去している。
     *
     * 設定されている場合、キャプチャバックエンドは CPU 側の色空間変換・
     * メモリコピー (Map/memcpy/sws_scale) を行わずに GPU テクスチャを
     * そのまま引き渡しており、data は空のままでよい。エンコーダー側は
     * このテクスチャを直接ハードウェアエンコーダーへ渡すゼロコピーパスを
     * 使用できる。nullptr の場合は通常の CPU パス（data を使用）になる。
     */
    std::shared_ptr<void> gpu_texture;
};

/**
 * @brief フレームメタデータ
 *
 * フレームのシーケンス番号・タイムスタンプ・解像度を保持する。
 */
struct FrameMeta {
    uint64_t sequence     = 0;   ///< フレームシーケンス番号
    int64_t  timestamp_us = 0;   ///< タイムスタンプ (マイクロ秒)
    uint32_t width        = 0;   ///< 幅 (ピクセル)
    uint32_t height       = 0;   ///< 高さ (ピクセル)
};

/**
 * @brief エンコード済みパケット
 *
 * FFmpeg でエンコードされたパケットデータを保持する。
 */
struct EncodedPacket {
    std::vector<uint8_t> data;   ///< エンコード済みバイト列
    int64_t pts         = 0;     ///< プレゼンテーションタイムスタンプ
    int64_t dts         = 0;     ///< デコードタイムスタンプ
    int64_t duration    = 0;     ///< パケットの継続時間
    bool is_key_frame   = false; ///< キーフレームフラグ
    int  time_base_num  = 1;     ///< タイムベース分子
    int  time_base_den  = 60;    ///< タイムベース分母
};

/**
 * @brief エンコーダー設定
 *
 * 使用するコーデックや映像品質に関するパラメータを保持する。
 */
struct EncoderConfig {
    std::string codec                        = "h264_nvenc";       ///< 優先コーデック
    std::vector<std::string> fallback_codecs = {"h264_mf", "libx264"};  ///< フォールバックコーデックリスト
    int bitrate_kbps  = 4000;      ///< ターゲットビットレート (kbps)
    int fps           = 60;        ///< フレームレート
    int gop_size      = 60;        ///< GOP サイズ
    int max_b_frames  = 0;         ///< 最大 B フレーム数
    std::string preset = "fast";   ///< エンコードプリセット
    std::string tune   = "zerolatency";  ///< チューニングオプション
    int threads        = 0;        ///< エンコードスレッド数 (0=自動)
};

/**
 * @brief RTSP 配信設定
 *
 * RTSP サーバーへの接続・再接続に関するパラメータを保持する。
 */
struct RtspConfig {
    std::string base_url;                           ///< RTSP サーバーのベース URL (rtsp://host:port)
    std::string path_pattern = "/live/screen{n}";   ///< ストリームパスパターン ({n} がモニター番号に置換)
    int connect_timeout_ms          = 5000;          ///< 接続タイムアウト (ms)
    int send_timeout_ms             = 5000;          ///< 送信タイムアウト (ms)
    int reconnect_delay_ms          = 1000;          ///< 再接続初期待機時間 (ms)
    int reconnect_max_delay_ms      = 30000;         ///< 再接続最大待機時間 (ms)
    float reconnect_backoff_multiplier = 2.0f;       ///< バックオフ乗数
};

/**
 * @brief 指数バックオフ状態
 *
 * 再接続時の待機時間を指数的に増加させるための状態管理構造体。
 */
struct BackoffState {
    int attempt  = 0;     ///< 試行回数
    int delay_ms = 1000;  ///< 現在の待機時間 (ms)

    /**
     * @brief バックオフをリセットする
     * @param initial_ms 初期待機時間 (ms)
     */
    void reset(int initial_ms = 1000) {
        attempt  = 0;
        delay_ms = initial_ms;
    }

    /**
     * @brief 次の遅延時間 (ms) を返し、内部状態を進める
     * @param max_ms     最大待機時間 (ms)
     * @param multiplier バックオフ乗数
     * @return 今回の待機時間 (ms)
     */
    int next_delay(int max_ms = 30000, float multiplier = 2.0f) {
        int d    = delay_ms;
        auto next = static_cast<int64_t>(
            std::min(static_cast<double>(delay_ms) * multiplier,
                     static_cast<double>(max_ms)));
        delay_ms = static_cast<int>(next);
        ++attempt;
        return d;
    }
};

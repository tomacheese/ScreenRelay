#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief FFmpeg H.264 エンコーダーのコントローラー
 *
 * h264_nvenc → h264_mf → libx264 の順でフォールバックを試みる。
 * 入力フレームフォーマットは BGRA を前提とする。
 */
class EncoderController {
public:
    /** @brief コンストラクタ */
    EncoderController();

    /** @brief デストラクタ。reset() を呼び出してリソースを解放する */
    ~EncoderController();

    /**
     * @brief エンコーダーを初期化する
     *
     * config.codec を優先コーデックとして試し、失敗した場合は
     * config.fallback_codecs の順でフォールバックを試みる。
     *
     * @param config   エンコーダー設定
     * @param width    フレーム幅 (px)
     * @param height   フレーム高さ (px)
     * @param error    エラーメッセージ出力先
     * @return 成功した場合 true
     */
    bool init(const EncoderConfig& config, uint32_t width, uint32_t height,
              std::string& error);

    /**
     * @brief BGRA フレームをエンコードする
     * @param frame       BGRA フレームバッファ
     * @param meta        フレームメタデータ
     * @param out_packets エンコード済みパケットの出力先
     * @return 成功した場合 true
     */
    bool encode(const FrameBuffer& frame, const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets);

    /**
     * @brief エンコーダーをフラッシュする
     *
     * ストリーム終端で残留フレームを取り出す際に使用する。
     *
     * @param out_packets フラッシュ済みパケットの出力先
     * @return 成功した場合 true
     */
    bool flush(std::vector<EncodedPacket>& out_packets);

    /**
     * @brief エンコーダーをリセットしてリソースを解放する
     *
     * init() を再度呼び出すことで再初期化できる。
     */
    void reset();

    /** @brief フレーム幅 (px) を返す */
    uint32_t width()  const { return width_; }

    /** @brief フレーム高さ (px) を返す */
    uint32_t height() const { return height_; }

    /**
     * @brief フレームレートを返す
     * @return 設定された fps 値
     */
    int fps() const;

    /**
     * @brief ビットレートを返す
     * @return 設定された bitrate_kbps 値
     */
    int bitrate_kbps() const;

    /**
     * @brief RTSP ストリーム初期化用のコーデック情報
     *
     * RtspPublisherClient::connect() に渡すために使用する。
     */
    struct CodecInfo {
        int codec_id      = 0;   ///< AVCodecID の値
        int width         = 0;   ///< フレーム幅 (px)
        int height        = 0;   ///< フレーム高さ (px)
        int fps           = 60;  ///< フレームレート
        int bit_rate      = 0;   ///< ビットレート (bps)
        int time_base_num = 1;   ///< タイムベース分子
        int time_base_den = 60;  ///< タイムベース分母
        std::vector<uint8_t> extradata;   ///< SPS/PPS などのエクストラデータ
        /** @brief RTCP SR の NTP 基点となる壁時計時刻 (μs, UNIX エポック起点)。
         *  0 の場合は接続時に av_gettime() で自動設定される。
         *  再接続時は最初のフレームの時刻を指定して RTCP SR の整合性を保つ。 */
        int64_t stream_start_us = 0;
    };

    /**
     * @brief コーデック情報を取得する
     * @return エンコーダーのコーデック情報
     */
    CodecInfo get_codec_info() const;

    /**
     * @brief 実際に選択されたコーデック名を返す
     *
     * フォールバックが発生した場合、config.codec ではなく
     * 実際に avcodec_open2() に成功したコーデック名を返す。
     * init() 前は空文字列を返す。
     *
     * @return 選択されたコーデック名
     */
    const std::string& selected_codec_name() const;

    /**
     * @brief 最初のフレームの壁時計時刻を返す
     *
     * RTSP 再接続時に start_time_realtime を正しく設定するために使用する。
     * まだフレームを受け取っていない場合は 0 を返す。
     *
     * @return μs 単位の時刻 (UNIX エポック起点)。未設定時は 0
     */
    int64_t first_frame_time_us() const;

private:
    /** Pimpl による実装詳細の隠蔽 */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint32_t width_  = 0;  ///< フレーム幅
    uint32_t height_ = 0;  ///< フレーム高さ
    EncoderConfig config_; ///< エンコーダー設定
};

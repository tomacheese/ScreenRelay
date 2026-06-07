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
     * shared_d3d11_device が指定され、かつ優先コーデックが NVENC である場合、
     * まず GPU ゼロコピーパス（D3D11VA ハードウェアフレームコンテキスト経由で
     * BGRA テクスチャを直接 NVENC に渡す方式）の確立を試みる。これにより
     * CPU 側での色空間変換 (sws_scale) とテクスチャ読み戻し (Map/memcpy) を
     * 完全に回避できる。失敗した場合は通常の CPU パスにフォールバックする。
     *
     * @param config              エンコーダー設定
     * @param width               フレーム幅 (px)
     * @param height              フレーム高さ (px)
     * @param error               エラーメッセージ出力先
     * @param shared_d3d11_device GPU ゼロコピーパスで共有する ID3D11Device* (型消去)。
     *                            nullptr の場合は CPU パスのみを使用する
     * @return 成功した場合 true
     */
    bool init(const EncoderConfig& config, uint32_t width, uint32_t height,
              std::string& error, void* shared_d3d11_device = nullptr);

    /**
     * @brief GPU ゼロコピーパスが有効かどうかを返す
     *
     * init() が GPU ハードウェアフレームコンテキスト経由での
     * エンコーダー初期化に成功した場合に true を返す。
     * true の場合、encode() に渡す FrameBuffer に gpu_texture が
     * 設定されていれば、CPU 変換を介さないゼロコピーパスが使用される。
     *
     * @return GPU ゼロコピーパスが有効なら true
     */
    bool is_gpu_zero_copy_active() const;

    /**
     * @brief BGRA フレームをエンコードする
     *
     * @param frame           BGRA フレームバッファ
     * @param meta            フレームメタデータ
     * @param out_packets     エンコード済みパケットの出力先
     * @param content_changed 直前に encode() したフレームからピクセル内容が
     *                        変化している場合は true。false を渡すと
     *                        sws_scale による BGRA→YUV 変換を省略し、
     *                        直前の変換結果（YUV フレーム）を再利用する。
     *                        静止画面のフリーズフレーム再送時に
     *                        CPU 負荷（色空間変換コスト）を大幅に削減できる。
     *                        省略した場合は true 扱い（常に変換する）。
     *                        GPU ゼロコピーパス使用時は GPU 側コピーが
     *                        十分に高速なため、このフラグに関わらず
     *                        毎フレームコピーを実行する。
     *
     * frame.gpu_texture が設定されており、かつ is_gpu_zero_copy_active() が
     * true の場合は GPU ゼロコピーパスを使用する（CPU 側の色空間変換を行わず、
     * GPU 上でテクスチャをハードウェアエンコーダーに直接渡す）。
     * それ以外の場合は frame.data を BGRA バッファとして扱い、
     * sws_scale で変換してからエンコードする（CPU パス）。
     *
     * @return 成功した場合 true
     */
    bool encode(const FrameBuffer& frame, const FrameMeta& meta,
                std::vector<EncodedPacket>& out_packets,
                bool content_changed = true);

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
    /**
     * @brief GPU ゼロコピーパスでフレームをエンコードする
     *
     * frame.gpu_texture が参照する D3D11 テクスチャを、ハードウェアフレーム
     * コンテキストのプールテクスチャへ GPU 内コピー (CopySubresourceRegion) し、
     * そのまま avcodec_send_frame に渡す。CPU 側の色空間変換・メモリコピーは
     * 一切発生しない。
     *
     * @param frame       GPU テクスチャを保持する FrameBuffer
     * @param meta        フレームメタデータ
     * @param out_packets エンコード済みパケット出力先
     * @return 成功した場合 true
     */
    bool encode_gpu_zero_copy(const FrameBuffer& frame, const FrameMeta& meta,
                              std::vector<EncodedPacket>& out_packets);

    /** Pimpl による実装詳細の隠蔽 */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint32_t width_  = 0;  ///< フレーム幅
    uint32_t height_ = 0;  ///< フレーム高さ
    EncoderConfig config_; ///< エンコーダー設定
};

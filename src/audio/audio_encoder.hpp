#pragma once
#include "common/types.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief FFmpeg AAC エンコーダーのコントローラー
 *
 * WASAPI から得られるインターリーブド PCM（int16 または float32）を
 * libswresample でエンコーダーの要求フォーマットへ変換し、AAC (LC) で
 * エンコードする。入力サンプルレート・チャンネル数が設定と異なる場合は
 * リサンプリングも行う。
 */
class AudioEncoderController {
public:
    /** @brief コンストラクタ */
    AudioEncoderController();

    /** @brief デストラクタ。reset() を呼び出してリソースを解放する */
    ~AudioEncoderController();

    /**
     * @brief エンコーダーを初期化する
     *
     * @param config          音声設定（bitrate_kbps / sample_rate / channels を使用）
     * @param in_sample_rate  入力 PCM のサンプルレート (Hz)
     * @param in_channels     入力 PCM のチャンネル数
     * @param in_is_float     入力 PCM が IEEE float32 の場合 true、int16 の場合 false
     * @param error           エラーメッセージ出力先
     * @return 成功した場合 true
     */
    bool init(const AudioConfig& config, int in_sample_rate, int in_channels,
              bool in_is_float, std::string& error);

    /**
     * @brief PCM バッファをエンコードする
     *
     * 内部 FIFO に蓄積し、エンコーダーの要求フレームサイズ分だけ溜まった時点で
     * エンコードを行う。1 回の呼び出しで 0 個以上のパケットが生成され得る。
     *
     * @param buf         入力 PCM バッファ（インターリーブド）
     * @param meta        入力バッファのメタデータ（タイムスタンプ）
     * @param out_packets エンコード済みパケットの出力先
     * @return 成功した場合 true
     */
    bool encode(const AudioBuffer& buf, const AudioMeta& meta,
                std::vector<EncodedPacket>& out_packets);

    /**
     * @brief エンコーダーをフラッシュする
     * @param out_packets フラッシュ済みパケットの出力先
     * @return 成功した場合 true
     */
    bool flush(std::vector<EncodedPacket>& out_packets);

    /** @brief エンコーダーをリセットしてリソースを解放する */
    void reset();

    /**
     * @brief RTSP ストリーム初期化用のコーデック情報を取得する
     * @return 音声コーデック情報
     */
    AudioCodecInfo get_codec_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // FIFO からのエンコード処理を分離するための内部ヘルパー (audio_encoder.cpp)
    friend bool drain_fifo_and_encode(Impl* impl, std::vector<EncodedPacket>& out_packets);
};

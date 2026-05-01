#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/types.hpp"
#include "encoder/encoder_controller.hpp"
#include <atomic>
#include <string>
#include <memory>

/**
 * @brief RTSP ANNOUNCE/RECORD クライアント
 *
 * FFmpeg の avformat を使用して RTSP サーバーにエンコード済みパケットを
 * TCP トランスポートで送信する。
 * 接続 URL は外部から渡され、ひとつのインスタンスがひとつの URL に対応する。
 */
class RtspPublisherClient {
public:
    /** @brief コンストラクタ */
    RtspPublisherClient();

    /** @brief デストラクタ。disconnect() を呼び出してリソースを解放する */
    ~RtspPublisherClient();

    /**
     * @brief RTSP サーバーに接続する
     *
     * avformat_write_header() を呼び出して ANNOUNCE/RECORD を行う。
     * 接続後に send_packet() でパケットを送信できる。
     *
     * @param url        完全な RTSP URL (rtsp://host:port/path)
     * @param config     RTSP 設定 (タイムアウト等)
     * @param codec_info エンコーダーのコーデック情報
     * @param error      エラーメッセージ出力先
     * @return 成功した場合 true
     */
    bool connect(const std::string& url,
                 const RtspConfig& config,
                 const EncoderController::CodecInfo& codec_info,
                 std::string& error);

    /**
     * @brief エンコード済みパケットを送信する
     * @param pkt 送信するパケット
     * @return 成功した場合 true。失敗時は is_connected() が false になる
     */
    bool send_packet(const EncodedPacket& pkt);

    /**
     * @brief RTSP 接続を切断してリソースを解放する
     *
     * 二重呼び出しに対して安全。
     */
    void disconnect();

    /**
     * @brief 接続状態を返す
     * @return 接続中なら true
     */
    bool is_connected() const { return connected_.load(); }

private:
    /** Pimpl による実装詳細の隠蔽 */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> connected_{false};  ///< 接続状態フラグ
};

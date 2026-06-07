#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <optional>
#include "common/types.hpp"

/**
 * @brief キャプチャバックエンドの抽象インターフェース
 *
 * DXGI Duplication など、複数のキャプチャ手段に対して共通の操作を提供する。
 * 実装クラスはこのインターフェースを継承して各バックエンド固有の処理を記述する。
 */
class ICaptureBackend {
public:
    /** デストラクタ。派生クラスのリソースを確実に解放するため virtual にする */
    virtual ~ICaptureBackend() = default;

    /**
     * @brief 指定モニターに対してキャプチャを初期化する
     * @param monitor          対象 HMONITOR
     * @param logical_width    論理解像度の幅
     * @param logical_height   論理解像度の高さ
     * @return 成功した場合 true
     */
    virtual bool init(HMONITOR monitor, int logical_width, int logical_height) = 0;

    /**
     * @brief 最新フレームを取得する
     * @param timeout_ms   タイムアウト (ms)
     * @return フレームバッファ。フレームなし (タイムアウト等) なら nullopt
     */
    virtual std::optional<FrameBuffer> acquire_frame(int timeout_ms) = 0;

    /**
     * @brief 解像度変更を通知して再構成する
     * @param new_width   新しい論理幅
     * @param new_height  新しい論理高さ
     * @return 成功した場合 true
     */
    virtual bool reconfigure(int new_width, int new_height) = 0;

    /**
     * @brief キャプチャリソースを解放する
     *
     * デストラクタからも呼ばれることを想定し、二重解放に対して安全であること。
     */
    virtual void release() = 0;

    /**
     * @brief 最後のエラーメッセージを返す
     *
     * スレッドセーフな実装が必要な場合は派生クラスでオーバーライドする。
     *
     * @return エラーメッセージ文字列のコピー
     */
    virtual std::string last_error() const {
        return {};
    }

    /**
     * @brief DXGI ACCESS_LOST 等、再初期化が必要なハードエラーが発生しているか返す
     *
     * タイムアウト（画面変化なし）とは区別する。
     * ハードエラーが発生したらフレームポンプはコンシューマーに即座に通知する。
     *
     * @return ハードエラーが発生していれば true
     */
    virtual bool has_hard_error() const { return false; }

    /**
     * @brief GPU ゼロコピーパスで共有する D3D11 デバイスを返す
     *
     * EncoderController が同一デバイス上にハードウェアフレームコンテキストを
     * 構築できるようにするために使用する。同一デバイスを共有することで、
     * キャプチャしたテクスチャをエンコーダーに渡す際のコピーが GPU 内で完結し、
     * CPU を介したデータ転送（Map/memcpy/sws_scale）を回避できる。
     *
     * @return ID3D11Device* (型消去)。GPU ゼロコピーをサポートしない
     *         バックエンドや未初期化の場合は nullptr
     */
    virtual void* gpu_device() const { return nullptr; }

    /**
     * @brief GPU ゼロコピーパスが利用可能かどうかを返す
     *
     * 物理解像度と論理解像度が一致しスケーリングが不要な場合のみ true を返す。
     * スケーリングが必要な場合は GPU 側での変換実装が必要になるため、
     * 現状では CPU パス（sws_scale）にフォールバックする。
     *
     * @return ゼロコピーパスが利用可能なら true
     */
    virtual bool supports_zero_copy() const { return false; }

    /**
     * @brief ゼロコピーモードの有効・無効を切り替える
     *
     * 有効化すると、acquire_frame() は CPU 側のメモリコピー・色空間変換を
     * 行わず、GPU テクスチャ参照のみを FrameBuffer::gpu_texture に設定する。
     * EncoderController が GPU ゼロコピーパスを確立できた場合にのみ
     * 呼び出し元（ScreenPipeline）が有効化する。
     *
     * @param enabled true で有効化、false で無効化（CPU パスへ復帰）
     */
    virtual void set_zero_copy_mode(bool enabled) { (void)enabled; }
};

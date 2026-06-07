#pragma once
#include "capture/capture_backend.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/**
 * @brief DXGI Desktop Duplication API を使用したキャプチャバックエンド
 *
 * Windows 8 以降で利用可能な IDXGIOutputDuplication を使い、
 * GPU からデスクトップ画面を直接取得する。
 * 取得した BGRA フレームは、物理解像度と論理解像度が異なる場合のみ
 * libswscale でダウンスケールして返す。
 */
class DxgiCaptureBackend : public ICaptureBackend {
public:
    /**
     * @brief コンストラクタ
     *
     * Impl を生成するのみ。実際の初期化は init() で行う。
     */
    DxgiCaptureBackend();

    /**
     * @brief デストラクタ
     *
     * release() を呼び出して COM リソースと sws_ctx を解放する。
     */
    ~DxgiCaptureBackend() override;

    /**
     * @brief 指定モニターに対してキャプチャを初期化する
     * @param monitor          対象 HMONITOR
     * @param logical_width    論理解像度の幅
     * @param logical_height   論理解像度の高さ
     * @return 成功した場合 true
     */
    bool init(HMONITOR monitor, int logical_width, int logical_height) override;

    /**
     * @brief 最新フレームを取得する
     * @param timeout_ms   タイムアウト (ms)
     * @return フレームバッファ。タイムアウト時は nullopt
     */
    std::optional<FrameBuffer> acquire_frame(int timeout_ms) override;

    /**
     * @brief 解像度変更を通知して sws_ctx を再構成する
     *
     * DXGI Duplication 自体は継続使用し、スケーラーのみ再作成する。
     *
     * @param new_width   新しい論理幅
     * @param new_height  新しい論理高さ
     * @return 常に true
     */
    bool reconfigure(int new_width, int new_height) override;

    /**
     * @brief すべての COM リソースと sws_ctx を解放する
     *
     * 二重呼び出しに対して安全。
     */
    void release() override;

    /**
     * @brief 最後のエラーメッセージを取得する
     *
     * スレッドセーフ。acquire_frame() と並行して呼び出し可能。
     *
     * @return エラーメッセージ文字列のコピー
     */
    std::string last_error() const override {
        std::lock_guard<std::mutex> lk(last_error_mutex_);
        return last_error_;
    }

    /**
     * @brief DXGI ACCESS_LOST 等のハードエラーが発生しているか返す
     *
     * init() を呼び出すとフラグはリセットされる。スレッドセーフ。
     *
     * @return ハードエラーが発生していれば true
     */
    bool has_hard_error() const override { return hard_error_.load(); }

    /**
     * @brief キャプチャに使用している D3D11 デバイスを返す
     *
     * EncoderController が GPU ゼロコピーパス用のハードウェアフレーム
     * コンテキストを同一デバイス上に構築するために使用する。
     *
     * @return ID3D11Device* (型消去)。未初期化の場合は nullptr
     */
    void* gpu_device() const override;

    /**
     * @brief GPU ゼロコピーパスが利用可能かどうかを返す
     *
     * 物理解像度と論理解像度が一致し、sws_ctx によるスケーリングが
     * 不要な場合のみ true を返す。
     *
     * @return ゼロコピーパスが利用可能なら true
     */
    bool supports_zero_copy() const override;

    /**
     * @brief ゼロコピーモードの有効・無効を切り替える
     *
     * 有効化すると acquire_frame() は CPU 側のメモリコピーを行わず、
     * GPU テクスチャ参照のみを返す。
     *
     * @param enabled true で有効化、false で無効化
     */
    void set_zero_copy_mode(bool enabled) override;

private:
    /**
     * @brief HMONITOR に対応する DXGI アダプターとアウトプットを検索して初期化する
     * @param monitor  対象 HMONITOR
     * @return 成功した場合 true
     */
    bool find_and_init(HMONITOR monitor);

    /** Pimpl による実装詳細の隠蔽 */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    int logical_width_  = 0;   ///< 論理解像度の幅
    int logical_height_ = 0;   ///< 論理解像度の高さ

    mutable std::mutex last_error_mutex_;           ///< last_error_ 保護用ミューテックス
    std::string        last_error_;                 ///< 最後のエラーメッセージ (last_error_mutex_ 保護下)
    std::atomic<bool>  hard_error_{false};          ///< ACCESS_LOST 等の再初期化が必要なエラーフラグ
    std::atomic<bool>  zero_copy_mode_{false};      ///< GPU ゼロコピーモード有効フラグ
};

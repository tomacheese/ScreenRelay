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
     * @return エラーメッセージ文字列
     */
    virtual const std::string& last_error() const {
        static const std::string empty{};
        return empty;
    }
};

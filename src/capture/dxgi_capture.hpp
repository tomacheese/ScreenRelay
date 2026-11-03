#pragma once
#include "capture/capture_backend.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/**
 * @brief Capture backend using the DXGI Desktop Duplication API.
 *
 * Grabs the desktop directly from the GPU via IDXGIOutputDuplication
 * (Windows 8+). BGRA frames are downscaled with libswscale only when the
 * physical and logical resolutions differ.
 */
class DxgiCaptureBackend : public ICaptureBackend {
public:
    /** Only creates Impl; actual initialization happens in init(). */
    DxgiCaptureBackend();

    /** Calls release() to free COM resources and sws_ctx. */
    ~DxgiCaptureBackend() override;

    bool init(HMONITOR monitor, int logical_width, int logical_height) override;

    /** @return The frame, or nullopt on timeout or error */
    std::optional<FrameBuffer> acquire_frame(int timeout_ms) override;

    /**
     * @brief Applies a new logical resolution by recreating the scaler only.
     *
     * The DXGI Duplication itself keeps being used.
     *
     * @return Always true
     */
    bool reconfigure(int new_width, int new_height) override;

    /** Releases all COM resources and sws_ctx. Safe to call repeatedly. */
    void release() override;

    /** Thread-safe; may be called concurrently with acquire_frame(). */
    std::string last_error() const override {
        std::lock_guard<std::mutex> lk(last_error_mutex_);
        return last_error_;
    }

    /** Thread-safe. The flag is reset by init(). */
    bool has_hard_error() const override { return hard_error_.load(); }

    /**
     * @brief Returns the D3D11 device used for capture.
     * @return Type-erased ID3D11Device*, or nullptr if not initialized
     */
    void* gpu_device() const override;

    /** True only when no scaling is needed (sws_ctx is null). */
    bool supports_zero_copy() const override;

    /** While enabled, acquire_frame() returns only a GPU texture reference (no CPU copy). */
    void set_zero_copy_mode(bool enabled) override;

private:
    /**
     * @brief Finds the DXGI adapter/output matching the monitor and initializes capture on it.
     * @param monitor  Target HMONITOR
     * @return true on success
     */
    bool find_and_init(HMONITOR monitor);

    /** Pimpl: keeps D3D11/DXGI types out of this header. */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    int logical_width_  = 0;   ///< Logical width
    int logical_height_ = 0;   ///< Logical height

    mutable std::mutex last_error_mutex_;           ///< Guards last_error_
    std::string        last_error_;                 ///< Last error message (guarded by last_error_mutex_)
    std::atomic<bool>  hard_error_{false};          ///< Set on errors needing re-init (ACCESS_LOST etc.)
    std::atomic<bool>  zero_copy_mode_{false};      ///< GPU zero-copy mode enabled
};

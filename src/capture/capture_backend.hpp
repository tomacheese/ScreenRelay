#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <optional>
#include "common/types.hpp"

/**
 * @brief Abstract interface for screen capture backends (e.g. DXGI Duplication).
 */
class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    /**
     * @brief Initializes capture for the given monitor.
     * @param monitor          Target HMONITOR
     * @param logical_width    Logical (output) width
     * @param logical_height   Logical (output) height
     * @return true on success
     */
    virtual bool init(HMONITOR monitor, int logical_width, int logical_height) = 0;

    /**
     * @brief Acquires the latest frame.
     * @param timeout_ms   Timeout in milliseconds
     * @return The frame, or nullopt if no frame is available (timeout, error)
     */
    virtual std::optional<FrameBuffer> acquire_frame(int timeout_ms) = 0;

    /**
     * @brief Applies a new logical resolution.
     * @param new_width   New logical width
     * @param new_height  New logical height
     * @return true on success
     */
    virtual bool reconfigure(int new_width, int new_height) = 0;

    /**
     * @brief Releases capture resources.
     *
     * May also be called from the destructor, so it must be safe to call repeatedly.
     */
    virtual void release() = 0;

    /**
     * @brief Returns the last error message.
     *
     * Override when a thread-safe implementation is needed.
     *
     * @return A copy of the error message
     */
    virtual std::string last_error() const {
        return {};
    }

    /**
     * @brief Returns whether a hard error that requires re-initialization
     *        (e.g. DXGI ACCESS_LOST) has occurred.
     *
     * Distinct from a timeout (no screen change). On a hard error the frame
     * pump immediately notifies consumers.
     *
     * @return true if a hard error has occurred
     */
    virtual bool has_hard_error() const { return false; }

    /**
     * @brief Returns the D3D11 device shared with the GPU zero-copy path.
     *
     * EncoderController builds its hardware frame context on this same device,
     * so passing captured textures to the encoder stays inside the GPU
     * (no CPU Map/memcpy/sws_scale).
     *
     * @return Type-erased ID3D11Device*, or nullptr if the backend does not
     *         support zero-copy or is not initialized
     */
    virtual void* gpu_device() const { return nullptr; }

    /**
     * @brief Returns whether the GPU zero-copy path is available.
     *
     * True only when physical and logical resolutions match (no scaling).
     * GPU-side scaling is not implemented, so scaled captures fall back to
     * the CPU path (sws_scale).
     *
     * @return true if zero-copy is available
     */
    virtual bool supports_zero_copy() const { return false; }

    /**
     * @brief Enables or disables zero-copy mode.
     *
     * When enabled, acquire_frame() skips CPU copy and color conversion and
     * only sets a GPU texture reference in FrameBuffer::gpu_texture. The caller
     * (ScreenPipeline) enables it only after EncoderController has established
     * the GPU zero-copy path.
     *
     * @param enabled true to enable, false to return to the CPU path
     */
    virtual void set_zero_copy_mode(bool enabled) { (void)enabled; }
};

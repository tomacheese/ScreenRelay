#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>   // ID3D10Multithread (serializes access to the D3D11 immediate context)
#include <dxgi1_2.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "capture/dxgi_capture.hpp"
#include <cstring>
#include <string>

/** Implementation details (Pimpl) so D3D11/DXGI types stay out of the header. */
struct DxgiCaptureBackend::Impl {
    ID3D11Device*             device         = nullptr;
    ID3D11DeviceContext*      context        = nullptr;
    IDXGIOutputDuplication*   duplication    = nullptr;
    ID3D11Texture2D*          staging_tex    = nullptr;  ///< CPU-readable staging texture
    SwsContext*               sws_ctx        = nullptr;  ///< Non-null only when scaling is needed
    int     physical_width  = 0;   ///< Physical (desktop) width
    int     physical_height = 0;   ///< Physical (desktop) height
    uint64_t frame_seq      = 0;

    /**
     * @brief Texture pool for the GPU zero-copy path.
     *
     * The texture from AcquireNextFrame is invalid after ReleaseFrame(), so
     * it is copied (CopyResource, GPU-only) into a pool texture. The pool is
     * a ring buffer so that older references still held by a freeze-frame
     * cache or the frame queue are not overwritten by new captures
     * (the immediate context executes commands in submission order, so
     * read-before-write ordering on a subresource is preserved).
     */
    static constexpr int kZeroCopyPoolSize = 6;
    ID3D11Texture2D* zero_copy_pool[kZeroCopyPoolSize] = {};
    int  zero_copy_pool_idx = 0;                              ///< Next pool slot to use

    /**
     * @brief Multithread-protection interface of the D3D11 immediate context.
     *
     * FFmpeg binds d3d11_hwctx->lock/unlock to ID3D10Multithread::Enter/Leave
     * in av_hwdevice_ctx_init. Taking the same lock around CopyResource on the
     * FramePump thread excludes the encoder thread's CopySubresourceRegion
     * (the immediate context is not thread-safe).
     */
    ID3D10Multithread* multithread = nullptr;
};

DxgiCaptureBackend::DxgiCaptureBackend()
    : impl_(std::make_unique<Impl>()) {}

DxgiCaptureBackend::~DxgiCaptureBackend() {
    release();
}

bool DxgiCaptureBackend::init(HMONITOR monitor, int logical_width, int logical_height) {
    hard_error_.store(false);
    logical_width_  = logical_width;
    logical_height_ = logical_height;

    if (!find_and_init(monitor)) {
        // Free any partially initialized resources (device, duplication,
        // staging texture, zero-copy pool, ...). find_and_init() has already
        // released the factory and adapter.
        release();
        return false;
    }

    if (impl_->physical_width != logical_width || impl_->physical_height != logical_height) {
        impl_->sws_ctx = sws_getContext(
            impl_->physical_width,  impl_->physical_height, AV_PIX_FMT_BGRA,
            logical_width,          logical_height,          AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!impl_->sws_ctx) {
            last_error_ = "sws_getContext failed";
            release();
            return false;
        }
    }

    return true;
}

/**
 * Flow: AcquireNextFrame -> copy to staging texture -> Map -> FrameBuffer.
 * Downscales with sws_scale when physical and logical resolutions differ.
 * In zero-copy mode, only copies into the GPU texture pool and returns a reference.
 */
std::optional<FrameBuffer> DxgiCaptureBackend::acquire_frame(int timeout_ms) {
    if (!impl_->duplication) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "Not initialized";
        }
        return std::nullopt;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    IDXGIResource*           desktop_resource = nullptr;

    HRESULT hr = impl_->duplication->AcquireNextFrame(
        static_cast<UINT>(timeout_ms),
        &frame_info,
        &desktop_resource
    );

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Not an error: the screen did not change.
        return std::nullopt;
    }

    if (FAILED(hr)) {
        // Unrecoverable (e.g. ACCESS_LOST). Write last_error_ before raising
        // the atomic flag so a consumer that sees the flag also sees the message.
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "AcquireNextFrame failed: HRESULT=" + std::to_string(static_cast<long>(hr));
        }
        hard_error_.store(true);
        return std::nullopt;
    }

    ID3D11Texture2D* tex = nullptr;
    hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    desktop_resource->Release();
    desktop_resource = nullptr;

    if (FAILED(hr) || !tex) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "QueryInterface for ID3D11Texture2D failed";
        }
        impl_->duplication->ReleaseFrame();
        return std::nullopt;
    }

    // Zero-copy mode: copy on the GPU into the texture pool and return only a
    // reference (no Map/memcpy/sws_scale).
    if (zero_copy_mode_.load()) {
        ID3D11Texture2D* dst = impl_->zero_copy_pool[impl_->zero_copy_pool_idx];
        impl_->zero_copy_pool_idx = (impl_->zero_copy_pool_idx + 1) % Impl::kZeroCopyPoolSize;

        // Take the same lock as FFmpeg's d3d11_hwctx->lock (ID3D10Multithread::Enter)
        // so this CopyResource is serialized with the encoder thread's
        // CopySubresourceRegion; concurrent immediate-context access is undefined behavior.
        if (impl_->multithread) impl_->multithread->Enter();
        impl_->context->CopyResource(dst, tex);
        if (impl_->multithread) impl_->multithread->Leave();
        tex->Release();
        tex = nullptr;
        impl_->duplication->ReleaseFrame();

        // AddRef so the handle owns a reference; the deleter's Release() drops
        // only that reference and the texture itself stays in the pool.
        dst->AddRef();
        std::shared_ptr<void> handle(dst, [](void* p) {
            static_cast<ID3D11Texture2D*>(p)->Release();
        });

        FrameBuffer fb;
        fb.width       = static_cast<uint32_t>(impl_->physical_width);
        fb.height      = static_cast<uint32_t>(impl_->physical_height);
        fb.format      = PixelFormat::BGRA;
        fb.gpu_texture = std::move(handle);
        return fb;
    }

    impl_->context->CopyResource(impl_->staging_tex, tex);
    tex->Release();
    tex = nullptr;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = impl_->context->Map(impl_->staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lk(last_error_mutex_);
            last_error_ = "ID3D11DeviceContext::Map failed";
        }
        impl_->duplication->ReleaseFrame();
        return std::nullopt;
    }

    const int phys_w = impl_->physical_width;
    const int phys_h = impl_->physical_height;

    if (impl_->sws_ctx) {
        FrameBuffer fb;
        fb.width  = static_cast<uint32_t>(logical_width_);
        fb.height = static_cast<uint32_t>(logical_height_);
        fb.format = PixelFormat::BGRA;
        fb.data.resize(static_cast<size_t>(logical_width_) * static_cast<size_t>(logical_height_) * 4u);

        const uint8_t* src_data[1] = { static_cast<const uint8_t*>(mapped.pData) };
        int            src_stride[1] = { static_cast<int>(mapped.RowPitch) };

        uint8_t* dst_data[1]  = { fb.data.data() };
        int      dst_stride[1] = { logical_width_ * 4 };

        sws_scale(
            impl_->sws_ctx,
            src_data,   src_stride, 0, phys_h,
            dst_data,   dst_stride
        );

        impl_->context->Unmap(impl_->staging_tex, 0);
        impl_->duplication->ReleaseFrame();

        return fb;
    } else {
        FrameBuffer fb;
        fb.width  = static_cast<uint32_t>(phys_w);
        fb.height = static_cast<uint32_t>(phys_h);
        fb.format = PixelFormat::BGRA;
        fb.data.resize(static_cast<size_t>(phys_w) * static_cast<size_t>(phys_h) * 4u);

        // RowPitch may be padded beyond width*4, in which case copy row by row.
        const size_t row_bytes = static_cast<size_t>(phys_w) * 4u;
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        uint8_t*       dst = fb.data.data();

        if (mapped.RowPitch == static_cast<UINT>(phys_w * 4)) {
            std::memcpy(dst, src, fb.data.size());
        } else {
            for (int row = 0; row < phys_h; ++row) {
                std::memcpy(dst + row * row_bytes,
                            src + row * mapped.RowPitch,
                            row_bytes);
            }
        }

        impl_->context->Unmap(impl_->staging_tex, 0);
        impl_->duplication->ReleaseFrame();

        return fb;
    }
}

bool DxgiCaptureBackend::reconfigure(int new_width, int new_height) {
    logical_width_  = new_width;
    logical_height_ = new_height;

    if (impl_->sws_ctx &&
        impl_->physical_width == new_width &&
        impl_->physical_height == new_height)
    {
        // Scaling is no longer needed.
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    } else if (impl_->physical_width != 0) {
        // Recreate the scaler for the new size.
        if (impl_->sws_ctx) {
            sws_freeContext(impl_->sws_ctx);
            impl_->sws_ctx = nullptr;
        }

        if (impl_->physical_width != new_width || impl_->physical_height != new_height) {
            impl_->sws_ctx = sws_getContext(
                impl_->physical_width,  impl_->physical_height, AV_PIX_FMT_BGRA,
                new_width,              new_height,              AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
        }
    }

    return true;
}

void DxgiCaptureBackend::release() {
    if (!impl_) {
        return;
    }

    if (impl_->sws_ctx) {
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    }

    if (impl_->staging_tex) {
        impl_->staging_tex->Release();
        impl_->staging_tex = nullptr;
    }

    for (int i = 0; i < Impl::kZeroCopyPoolSize; ++i) {
        if (impl_->zero_copy_pool[i]) {
            impl_->zero_copy_pool[i]->Release();
            impl_->zero_copy_pool[i] = nullptr;
        }
    }
    impl_->zero_copy_pool_idx = 0;
    zero_copy_mode_.store(false);

    // Release before the device context and device.
    if (impl_->multithread) {
        impl_->multithread->Release();
        impl_->multithread = nullptr;
    }

    if (impl_->duplication) {
        impl_->duplication->Release();
        impl_->duplication = nullptr;
    }

    if (impl_->context) {
        impl_->context->Release();
        impl_->context = nullptr;
    }

    if (impl_->device) {
        impl_->device->Release();
        impl_->device = nullptr;
    }
}

void* DxgiCaptureBackend::gpu_device() const {
    return impl_->device;
}

/** Returns false when sws_ctx exists, since GPU-side scaling is not implemented. */
bool DxgiCaptureBackend::supports_zero_copy() const {
    return impl_->device != nullptr && impl_->sws_ctx == nullptr;
}

void DxgiCaptureBackend::set_zero_copy_mode(bool enabled) {
    zero_copy_mode_.store(enabled);
}

/**
 * Enumerates adapters via IDXGIFactory1, finds the output whose monitor
 * handle matches, and creates the D3D11 device, IDXGIOutputDuplication,
 * staging texture and zero-copy pool on that adapter.
 */
bool DxgiCaptureBackend::find_and_init(HMONITOR hmonitor) {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        last_error_ = "CreateDXGIFactory1 failed";
        return false;
    }

    bool found = false;

    for (UINT adapter_idx = 0; ; ++adapter_idx) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(adapter_idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT output_idx = 0; ; ++output_idx) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(output_idx, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC desc{};
            hr = output->GetDesc(&desc);

            if (SUCCEEDED(hr) && desc.Monitor == hmonitor) {
                D3D_FEATURE_LEVEL feature_level{};
                hr = D3D11CreateDevice(
                    adapter,
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    0,
                    nullptr, 0,
                    D3D11_SDK_VERSION,
                    &impl_->device,
                    &feature_level,
                    &impl_->context
                );

                if (FAILED(hr) || !impl_->device || !impl_->context) {
                    last_error_ = "D3D11CreateDevice failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    output->Release();
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // Enable multithread protection and cache the interface. The
                // FramePump thread's CopyResource and the encoder thread's
                // CopySubresourceRegion hit the same context concurrently, so
                // both must take the ID3D10Multithread::Enter/Leave lock that
                // FFmpeg uses as d3d11_hwctx->lock.
                {
                    ID3D10Multithread* mt = nullptr;
                    HRESULT mt_hr = impl_->device->QueryInterface(
                        __uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt));
                    if (SUCCEEDED(mt_hr) && mt) {
                        mt->SetMultithreadProtected(TRUE);
                        impl_->multithread = mt;
                        // Not released here; release() does it.
                    }
                }

                IDXGIOutput1* output1 = nullptr;
                hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                output->Release();
                output = nullptr;

                if (FAILED(hr) || !output1) {
                    last_error_ = "QueryInterface for IDXGIOutput1 failed";
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                hr = output1->DuplicateOutput(impl_->device, &impl_->duplication);
                output1->Release();
                output1 = nullptr;

                if (FAILED(hr) || !impl_->duplication) {
                    last_error_ = "DuplicateOutput failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                DXGI_OUTDUPL_DESC dup_desc{};
                impl_->duplication->GetDesc(&dup_desc);
                impl_->physical_width  = static_cast<int>(dup_desc.ModeDesc.Width);
                impl_->physical_height = static_cast<int>(dup_desc.ModeDesc.Height);

                // Staging texture for CPU readback.
                D3D11_TEXTURE2D_DESC tex_desc{};
                tex_desc.Width              = dup_desc.ModeDesc.Width;
                tex_desc.Height             = dup_desc.ModeDesc.Height;
                tex_desc.MipLevels          = 1;
                tex_desc.ArraySize          = 1;
                tex_desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
                tex_desc.SampleDesc.Count   = 1;
                tex_desc.SampleDesc.Quality = 0;
                tex_desc.Usage              = D3D11_USAGE_STAGING;
                tex_desc.BindFlags          = 0;
                tex_desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
                tex_desc.MiscFlags          = 0;

                hr = impl_->device->CreateTexture2D(&tex_desc, nullptr, &impl_->staging_tex);
                if (FAILED(hr) || !impl_->staging_tex) {
                    last_error_ = "CreateTexture2D (staging) failed: HRESULT=" +
                                  std::to_string(static_cast<long>(hr));
                    adapter->Release();
                    factory->Release();
                    return false;
                }

                // Zero-copy pool: D3D11_USAGE_DEFAULT, GPU-only (no CPU access).
                // SHADER_RESOURCE bind flag lets the encoder's hardware frame
                // context use these textures as a copy source.
                D3D11_TEXTURE2D_DESC zc_desc = tex_desc;
                zc_desc.Usage          = D3D11_USAGE_DEFAULT;
                zc_desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
                zc_desc.CPUAccessFlags = 0;

                for (int i = 0; i < Impl::kZeroCopyPoolSize; ++i) {
                    hr = impl_->device->CreateTexture2D(&zc_desc, nullptr, &impl_->zero_copy_pool[i]);
                    if (FAILED(hr) || !impl_->zero_copy_pool[i]) {
                        last_error_ = "CreateTexture2D (zero-copy pool) failed: HRESULT=" +
                                      std::to_string(static_cast<long>(hr));
                        adapter->Release();
                        factory->Release();
                        return false;
                    }
                }
                impl_->zero_copy_pool_idx = 0;

                found = true;
                adapter->Release();
                factory->Release();
                return true;
            }

            if (output) {
                output->Release();
                output = nullptr;
            }
        }

        adapter->Release();
    }

    factory->Release();

    if (!found) {
        last_error_ = "No matching DXGI output found for the specified monitor";
    }

    return false;
}

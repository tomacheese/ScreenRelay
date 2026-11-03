# screen-relay design

This document records the requirements, design decisions and known constraints behind screen-relay.
For the implemented structure see [architecture.md](./architecture.md) (components, data flow, thread model), [state-machine.md](./state-machine.md), [configuration.md](./configuration.md) and [metrics.md](./metrics.md).

---

## 1. Requirements

| Category | Item | Specification |
|---|---|---|
| Runtime | OS | Windows 10 / 11 (x64) |
| Runtime | GPU | NVIDIA GPU is optional (NVENC is used when available) |
| Runtime | Build | MSYS2 / MinGW-w64 (GCC 13+), CMake 3.20+ |
| Capture | API | DXGI Desktop Duplication (`capture.backend` accepts only `"dxgi"`) |
| Capture | Cursor | Not composited into the frames |
| Capture | Resolution | Logical resolution (after DPI scaling); a physical-resolution capture is downscaled with `sws_scale` (bilinear) when it differs |
| Capture | Frame rate | 60 fps by default (`encoder.fps`, 1-240) |
| Capture | Audio | Optional (`audio.enabled`, default off): WASAPI loopback or capture device, AAC, shared by all streams |
| Encode | Codec | `h264_nvenc` → `h264_mf` → `libx264`, falling back per monitor |
| Encode | Bitrate | 4000 kbps by default (`encoder.bitrate_kbps`) |
| Encode | GOP / B-frames | GOP = FPS value, B-frames = 0 (fixed for low latency) |
| Monitors | Count | Designed for up to 4 monitors (not enforced in code; the practical limit is the NVENC session limit, see R3) |
| Monitors | Numbering | Windows Settings display number (the `n` in `\\.\DISPLAYn`), recomputed on every poll, so a renumbering by Windows is followed within one poll |
| Monitors | Primary alias | The primary monitor is additionally published as `screen0`, which always points to the current primary and follows a primary change within one poll |
| Monitors | Change detection | Polling (1 second by default); a connected monitor gets a pipeline, a disconnected one has its pipeline stopped |
| Monitors | Pipeline key | Stable physical-monitor ID from the CCD API (`monitorDevicePath`) |
| Monitors | Mirroring | Mirroring-driver devices are excluded; only the source display is published |
| Delivery | Protocol | RTSP ANNOUNCE/RECORD over TCP (`rtsp_transport=tcp`) |
| Delivery | Path pattern | `rtsp.path_pattern`, code default `/live/screen{n}` (`config/config.example.json` uses `/screen{n}`) |
| Delivery | Latency target | 3 seconds or less (a design target; latency is not measured) |
| Delivery | Reconnect | Automatic reconnect with exponential backoff |
| Fault handling | GPU loss / DXGI hard error | Capture is re-initialized (`RECONFIGURING`); if that fails the pipeline goes to `FATAL` and the supervisor recreates it on the next poll |
| Libraries | FFmpeg | Shared LGPL build (dynamic linking) |
| Logging | Format | JSON Lines via spdlog (file with rotation, 10 MB × 5, plus console), configurable level |
| Metrics | Output | `metrics.json` (state, resolution, fps, bitrate, codec, frame / error counters) and `health.json` |
| UI | Form | CLI only |
| Distribution | Form | ZIP with the exe and the FFmpeg DLLs |

---

## 2. Design decisions

### 2.1 Monitor identity and RTSP paths

- Monitors are enumerated with `EnumDisplayMonitors()` every `runtime.monitor_check_interval_ms`; the number is extracted from `MONITORINFOEX.szDevice` (`\\.\DISPLAY2` → 2) and the primary flag from `MONITORINFOF_PRIMARY`, both re-evaluated on every poll.
- Mirroring-driver devices are skipped via the `DISPLAY_DEVICE_MIRRORING_DRIVER` flag of `EnumDisplayDevices()`.
- The stable ID (`MonitorInfo::stable_id`) is the CCD `monitorDevicePath`, obtained with `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` + `DisplayConfigGetDeviceInfo`.
  When the CCD lookup fails, `device_name` is used instead.
- `MonitorSupervisor::pipelines_` is keyed by `stable_id`, so a number repack by Windows does not break the mapping between a physical monitor and its pipeline.
- `apply_monitor_changes()` handles these cases:

| Case | Action |
|---|---|
| `stable_id` no longer present | Pipeline stopped and destroyed (`monitor_removed`) |
| New `stable_id` | Pipeline created and started (`monitor_added`) |
| Existing pipeline is `FATAL` | Pipeline stopped and recreated (`pipeline_restart`) |
| `number` / `is_primary` / `handle` changed | Pipeline stopped and recreated (`pipeline_retarget`) |
| Only the logical resolution changed | `notify_resized()`; the pipeline goes through `RECONFIGURING` |

- The `/screen0` alias and the Settings-number paths are kept correct by the retarget case.
  For example, the primary monitor `\\.\DISPLAY2` publishes to `/live/screen2` and `/live/screen0` (with the default `path_pattern`), a non-primary `\\.\DISPLAY1` only to `/live/screen1`.
  If the primary moves to another monitor (for example because a monitor is powered off), both affected pipelines are recreated within one poll.

### 2.2 Capture

- `ICaptureBackend` (`src/capture/capture_backend.hpp`) abstracts the capture API; `DxgiCaptureBackend` is the only implementation.
- DXGI: `IDXGIFactory1` → adapters → outputs, matching `IDXGIOutputDesc::Monitor` against the `HMONITOR`, so multi-GPU systems work without extra adapter lookup.
  `IDXGIOutput1::DuplicateOutput` yields the duplication; each `AcquireNextFrame` result is copied to a staging texture and mapped to BGRA, or handed over as a GPU texture in zero-copy mode.
- Desktop Duplication only delivers a frame when the screen changes.
  A timeout is therefore normal, and `ScreenPipeline` re-encodes the last frame at the configured fps (freeze frame) so the stream does not stall on a static screen.
- `FramePump` runs the capture loop on its own thread and keeps a bounded queue (4 frames); when full, the oldest frame is dropped.
- A hard DXGI error (for example `ACCESS_LOST`) is reported separately from a timeout and sends the pipeline to `RECONFIGURING`.
  Initialization is retried up to 15 times at 200 ms intervals, because `DuplicateOutput` may return `E_ACCESSDENIED` while the driver is updating after a resolution change.
- GPU zero-copy: when the physical and logical resolutions match and the encoder is NVENC, the capture backend and the encoder share one D3D11 device and textures are passed to NVENC without CPU readback or `sws_scale`.
  If the zero-copy path cannot be established, the CPU path is used with the same codec.

### 2.3 Encoder

- Fallback chain `encoder.codec` → `encoder.fallback_codecs`, evaluated per monitor each time an encoder is initialized.
  A codec is skipped when FFmpeg does not provide it or `avcodec_open2` fails.
- CPU path: `sws_scale` BGRA → `YUV420P` (preferred) or `NV12`, chosen from the codec's pixel formats.
  The conversion is skipped for repeated freeze frames.
- Low-latency settings: `max_b_frames`, `gop_size` from the config; `tune` (`zerolatency`) only for software encoders (`libx264`); `rc=cbr` and `delay=0` for NVENC.
- On a resolution change the encoder is flushed, destroyed and created again (the pipeline passes through `RECONFIGURING` → `CAPTURING` → `CONNECTING`).

### 2.4 RTSP publishing

- `RtspPublisherClient` (FFmpeg `avformat`, RTSP muxer) publishes one stream; the primary monitor's pipeline owns two clients (`screen{n}` and `screen0`) fed with the same `EncodedPacket`s.
  If sending to either client fails, the pipeline goes to `RECONNECTING` and reconnects both.
- Reconnect uses exponential backoff (`rtsp.reconnect_delay_ms`, `rtsp.reconnect_max_delay_ms`, `rtsp.reconnect_backoff_multiplier`); after a successful reconnect an IDR frame is requested.
- Video PTS is derived from the capture wall-clock time (not a frame counter), and `start_time_realtime` is set so that RTCP SR carries a consistent NTP-RTP mapping.
  The shared audio stream's PTS is recomputed per connection from the same base.

---

## 3. Technical risks and mitigations

### R1: DXGI on multi-GPU systems (low)

- **Issue**: On laptops with an iGPU and a dGPU, monitors may be attached to different adapters.
- **Mitigation**: The adapter / output pair is found by matching the `HMONITOR` (see 2.2), and `DuplicateOutput` is called on that adapter's device.

### R2: Reliability of Windows display numbers (mitigated, low)

- **Issue**: The `n` in `\\.\DISPLAYn` can change when a monitor is powered off, disconnected or after a reboot (Windows repacks the numbers).
- **Mitigation**: Pipelines are keyed by the CCD `monitorDevicePath`; the number and the primary flag are recomputed every poll and a change retargets the pipeline.
- **Residual risk**: When the CCD API fails, `device_name` is the fallback key, so a configuration where device names get repacked may occasionally see a wrong number assignment.
  Such failures are limited to legacy GPUs or unusual driver environments.

### R3: NVENC session limit (known, handled)

- **Issue**: Consumer GPUs allow only 2-3 concurrent NVENC sessions.
- **Mitigation**: `h264_nvenc` is tried per monitor and the next codec in the chain is used on failure, so mixed encoders across monitors are accepted.

### R4: DRM-protected content (accepted)

- **Issue**: Desktop Duplication returns black for DRM-protected content (a Windows security feature).
- **Mitigation**: Accepted as a limitation.

### R5: WGC backend (not implemented)

An earlier plan included a Windows Graphics Capture backend selectable through `capture.backend`.
It was never implemented; only `dxgi` is accepted.
C++/WinRT header compatibility with MinGW remains the main unknown if it is revisited.

# Architecture

## Overview

screen-relay captures every connected monitor with the Windows DXGI Desktop Duplication API, encodes each one as H.264, and publishes it over RTSP.

---

## System diagram

```text
┌──────────────────────────────────────────────────────────────────────┐
│   MonitorSupervisor                                                  │
│   ┌──────────────────────────────────────────────────────────────┐   │
│   │  monitor_poll_loop              metrics_writer_loop          │   │
│   │  (detects monitor changes       (writes JSON every           │   │
│   │   every monitor_check_          emit_metrics_interval_ms)    │   │
│   │   interval_ms)                                               │   │
│   └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌─────────────────────┐   ┌─────────────────────┐                  │
│  │  ScreenPipeline (1) │   │  ScreenPipeline (2) │   ...            │
│  │  DISPLAY1           │   │  DISPLAY2           │                  │
│  │  3840x2160          │   │  1280x1024          │                  │
│  └──────────┬──────────┘   └─────────────────────┘                  │
└─────────────│────────────────────────────────────────────────────────┘
              │
              ▼
   ┌──────────────────────┐
   │  DxgiCaptureBackend  │
   │  IDXGIOutputDupl     │
   └──────────┬───────────┘
              │ BGRA frame (or GPU texture in zero-copy mode)
              ▼
   ┌──────────────────────┐
   │   FramePump          │
   │   (bounded queue,    │
   │    max 4 frames)     │
   └──────────┬───────────┘
              │
              ▼
   ┌──────────────────────┐
   │  EncoderController   │
   │  nvenc→mf→libx264    │
   │  (FFmpeg + swscale)  │
   └──────────┬───────────┘
              │ H.264 NAL packets
              ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │  RtspPublisherClient × N (one per URL; two for the primary)      │
   │  /screen1  (+ /screen0 for the primary)                          │
   └──────────┬─────────────────────────────────────────────────────┘
              │ RTP over TCP (interleaved)
              ▼
   ┌──────────────────────┐
   │   MediaMTX (etc.)    │
   │   RTSP server        │
   └──────────────────────┘
```

---

## Components

| Component | Class | Role |
|---|---|---|
| Orchestrator | `MonitorSupervisor` | Detects monitor changes and manages the `ScreenPipeline` lifecycle |
| Pipeline | `ScreenPipeline` | Capture → encode → publish for one monitor |
| State machine | `StateMachine` | Enforces the transition table and notifies via callback |
| Capture backend | `DxgiCaptureBackend` | Screen capture through the DXGI Desktop Duplication API |
| Capture queue | `FramePump` | Runs the capture thread; bounded queue of at most 4 frames |
| Encoder | `EncoderController` | FFmpeg H.264 encoding (NVENC / h264_mf / libx264); swscale BGRA→YUV on the CPU path |
| RTSP client | `RtspPublisherClient` | FFmpeg RTSP ANNOUNCE + SETUP + RECORD, RTP packet sending |
| Metrics | `MetricsStore` | Thread-safe counters; writes `health.json` / `metrics.json` |
| Logging | `LogSink` | spdlog-based JSON Lines log (file + console) |
| Config | `ConfigLoader` | Config loading and validation with nlohmann/json |
| Monitor detection | `MonitorDetector` | Monitor enumeration via `EnumDisplayMonitors` |
| Audio capture | `WasapiAudioCapture` | PCM capture from a loopback or capture device via WASAPI |
| Audio encoder | `AudioEncoderController` | FFmpeg AAC encoding; resampling with libswresample |
| Audio pipeline | `AudioPipeline` | Shared capture→encode thread; broadcasts to every `ScreenPipeline` |

---

## Data flow

```text
[DXGI IDXGIOutputDuplication]
       │
       │  AcquireNextFrame() → staging texture → Map() → BGRA bytes
       ▼
[FrameBuffer (BGRA)]
       │
       │  push (non-blocking; the oldest frame is dropped when the queue is full)
       ▼
[bounded queue (max=4)]
       │
       │  pop (pipeline thread)
       ▼
[sws_scale: BGRA → YUV420P/NV12]
       │
       │  avcodec_send_frame / avcodec_receive_packet
       ▼
[AVPacket (H.264 NAL)]
       │
       │  av_interleaved_write_frame
       ▼
[RTSP RTP/TCP → MediaMTX]
```

When the preferred codec is NVENC and the capture backend reports zero-copy support (physical resolution equals logical resolution, so no scaling), `EncoderController` tries to set up a GPU zero-copy path through a D3D11VA hardware frame context. If it succeeds, the captured texture goes straight to NVENC and the CPU readback and `sws_scale` steps are skipped; otherwise the CPU path above is used.

DXGI only delivers a frame when the screen changes, so the pipeline re-encodes the last frame at the configured `fps` (a "freeze frame") to keep the stream alive.

---

## Thread model

```text
Main thread (MonitorSupervisor::run)
│
├── monitor_poll_loop thread
│   └── Every monitor_check_interval_ms, detects monitor changes
│       (added / removed / resolution / primary change) and FATAL pipelines
│       → starts, stops or recreates ScreenPipelines
│
├── metrics_writer_loop thread
│   └── Writes health.json / metrics.json every emit_metrics_interval_ms
│
├── AudioPipeline threads (only when audio.enabled)
│
└── [per monitor]
    ScreenPipeline::pipeline_thread_func
    │
    ├── State machine loop
    │   └── Each state handler runs synchronously
    │
    └── Capture thread (FramePump)
        └── Polls DxgiCaptureBackend::acquire_frame()
            → pushes FrameBuffers into the queue
```

---

## Encoder fallback

Codecs are tried in the order `codec` → `fallback_codecs` from the config. If `avcodec_open2()` fails, the next codec is tried.

```text
avcodec_find_encoder("h264_nvenc")
      │
      ├── found → avcodec_open2() → success → NVENC used
      │                           └── failure → next
      └── not found → avcodec_find_encoder("h264_mf")
                        │
                        ├── found → avcodec_open2() → success → h264_mf used
                        │                           └── failure → next
                        └── not found → avcodec_find_encoder("libx264")
                                          │
                                          └── avcodec_open2() → libx264 used
```

If every codec fails, the pipeline transitions to `FATAL` (see [state-machine.md](./state-machine.md)).

---

## Primary monitor dual publishing

The primary monitor (the one with the `MONITORINFOF_PRIMARY` flag) owns two `RtspPublisherClient` instances.

```text
ScreenPipeline (primary=true)
├── RtspPublisherClient → rtsp://host:8554/screen2   (example: when \\.\DISPLAY2 is the primary)
└── RtspPublisherClient → rtsp://host:8554/screen0   (primary alias)
```

`/screen0` therefore always carries the primary monitor. Clients that want the primary monitor without knowing its number should use this URL.

**Following a primary switch**: if the primary moves to another monitor (power-off, disconnect, settings change), `MonitorSupervisor::apply_monitor_changes()` detects the `is_primary` change and recreates the affected pipelines. `/screen0` follows the new primary within `monitor_check_interval_ms` (default 1 second).

---

## Audio pipeline (when `audio.enabled: true`)

When `audio.enabled` is set, `MonitorSupervisor` creates a **single** `AudioPipeline` regardless of the number of monitors. It runs `WasapiAudioCapture` → `AudioEncoderController` (AAC) on background threads. Encoded packets are broadcast to every subscribed `ScreenPipeline` and published as the same audio on each monitor's RTSP streams (`/screen1`, `/screen2`, … including `/screen0`).

```text
┌───────────────────────────────────────────────┐
│  AudioPipeline (single instance, shared)       │
│  WasapiAudioCapture → AudioEncoderController   │
│  (WASAPI PCM)          (FFmpeg AAC)            │
└───────────────────────┬───────────────────────┘
                         │ broadcast
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
AudioSubscriberQueue  AudioSubscriberQueue  ...
        │                │
        ▼                ▼
ScreenPipeline(1)    ScreenPipeline(2)   ...
  └ RtspPublisherClient (video + audio)
```

### Audio pts recalculation

Because the audio pipeline is shared by all monitors, `EncodedPacket` carries `capture_timestamp_us` (absolute capture time, UNIX epoch) instead of a pts relative to a single stream. Each `ScreenPipeline` recomputes a connection-specific pts from `capture_timestamp_us` just before sending, using its own RTSP connection start time (`connect_wallclock_us`, the same base as the video `start_time_realtime`). This keeps the RTCP SR NTP-to-RTP mapping consistent between video and audio.

### Failure handling

- If the audio device cannot be opened or the AAC encoder fails to initialize, `MonitorSupervisor::init()` logs the error and continues with video only.
- If the capture device is lost while running (for example, it is unplugged), `AudioPipeline` stops audio only. Video pipelines are unaffected.

---

## Dependencies

| Library | Purpose | Linking |
|---|---|---|
| FFmpeg (avcodec/avformat/avutil/swscale/swresample) | Encoding and RTSP publishing | Dynamic (.dll) |
| nlohmann/json | Config parsing | Header-only (CMake `FetchContent`) |
| spdlog | Structured logging | CMake `FetchContent` |
| Direct3D 11 / DXGI / WASAPI | Screen and audio capture | Windows system libraries |

---

## Known technical constraints

- **RTSP transport**: TCP only (`rtsp_transport=tcp`); UDP is not supported.
- **Pixel format**: DXGI outputs BGRA (not RGBA). Use `AV_PIX_FMT_BGRA` when converting to YUV with swscale.
- **Capture backend**: only DXGI is implemented (no Windows Graphics Capture). Additional backends can be added by implementing the `ICaptureBackend` interface.
- **Monitor numbers**: `n` in the device name `\\.\DISPLAYn` is used and re-read on every poll so it follows the Windows settings. Pipelines are keyed by a stable ID from the CCD API (`monitorDevicePath`), so the mapping between pipelines and physical monitors survives number changes (OS renumbering, primary switch).
- **DRM content**: screens showing protected (DRM) content may cause DXGI Desktop Duplication to fail.

# screen-relay

A Windows application that captures all connected monitors via the DXGI Desktop Duplication API, encodes each as H.264 using FFmpeg (NVENC → h264_mf → libx264 fallback), and publishes them as individual RTSP streams to a server such as [MediaMTX](https://github.com/bluenviron/mediamtx).

## Features

- **Multi-monitor capture** — each display gets its own RTSP stream (`/screen1`, `/screen2`, …)
- **Primary monitor alias** — `/screen0` always points to the primary monitor
- **Hardware H.264 encoding** — prefers NVIDIA NVENC; falls back to `h264_mf` (Windows Media Foundation) then `libx264`
- **Dynamic monitor detection** — handles monitor connect/disconnect and resolution changes at runtime without restart
- **Automatic reconnection** — exponential back-off on DXGI, encoder, or RTSP failures; self-heals after a monitor power cycle
- **Structured JSON logging** — JSON Lines event log with timestamps
- **Runtime metrics** — live `health.json` / `metrics.json` written to disk every second
- **State-machine driven** — explicit per-monitor pipeline state transitions; no undefined behaviour on failure

## Requirements

### Runtime

| Component | Notes |
|-----------|-------|
| Windows 10 / 11 (x64) | Required |
| NVIDIA GPU | Optional — enables NVENC hardware H.264; falls back to `h264_mf` then `libx264` |
| FFmpeg shared libs | Bundled in the release zip (`avcodec`, `avformat`, `avutil`, `swscale`, `swresample`) |
| RTSP server | e.g. [MediaMTX](https://github.com/bluenviron/mediamtx) v1.x — runs separately as the stream receiver |

### Build

| Component | Version |
|-----------|---------|
| GCC (MinGW-w64) | ≥ 13 recommended |
| CMake | ≥ 3.20 |
| Ninja | any recent |
| FFmpeg dev files | [win64-lgpl-shared](https://github.com/BtbN/FFmpeg-Builds/releases), avcodec ≥ 62 |

## Quick Start

### 1. Download from Releases

1. Open the [latest release](https://github.com/tomacheese/screen-relay/releases/latest)
2. Download `screen-relay-vX.Y.Z-win64.zip`
3. Extract the zip to any folder (e.g. `C:\Tools\screen-relay\`)

The extracted folder contains:

- `screen-relay.exe` — the main executable
- `*.dll` — FFmpeg LGPL shared libraries (must stay in the same folder)
- `config.example.json` — annotated configuration template
- `README.md`

### 2. Create your config

Copy `config.example.json` to `config.json` in the same folder and set at minimum:

```json
{
  "rtsp": {
    "base_url": "rtsp://<mediamtx-host>:8554",
    "path_pattern": "/screen{n}"
  }
}
```

Replace `<mediamtx-host>` with the IP address or hostname of your [MediaMTX](https://github.com/bluenviron/mediamtx) server.

### 3. Run

```bat
screen-relay.exe --config config.json
```

Each monitor becomes available as an individual RTSP stream:

| Monitor | Stream URL |
|---------|-----------|
| Primary (e.g. DISPLAY1) | `rtsp://<host>:8554/screen1` and `rtsp://<host>:8554/screen0` |
| DISPLAY2 | `rtsp://<host>:8554/screen2` |
| DISPLAY3 | `rtsp://<host>:8554/screen3` |

Use VLC, FFplay, or any RTSP-capable player to verify:

```powershell
ffplay rtsp://<mediamtx-host>:8554/screen1
```

Press `Ctrl+C` to stop gracefully.

---

## Build from Source

```powershell
# 1. Place FFmpeg LGPL shared build at deps/ffmpeg/
#    Download from https://github.com/BtbN/FFmpeg-Builds/releases
#    (ffmpeg-master-latest-win64-lgpl-shared.zip → extract to deps/ffmpeg/)

# 2. Configure CMake with the MinGW toolchain
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DBUILD_TESTS=ON

# 3. Build
cmake --build build

# 4. Copy FFmpeg DLLs next to the executable
Copy-Item deps\ffmpeg\bin\*.dll build\

# 5. Edit config
cp config/config.example.json config/config.json
# Set rtsp.base_url

# 6. Start MediaMTX (or any RTSP server)
mediamtx.exe

# 7. Run
build/screen-relay.exe --config config/config.json
```

See [`docs/build.md`](docs/build.md) for detailed instructions.

## Usage

```powershell
screen-relay.exe --config <path-to-config.json>
screen-relay.exe --help
```

The process responds to `Ctrl+C` / `CTRL_CLOSE_EVENT` for graceful shutdown.

## Configuration

See [`config/config.example.json`](config/config.example.json) for a fully-annotated example.
Full reference: [`docs/configuration.md`](docs/configuration.md)

### Minimum viable config

```json
{
  "rtsp": {
    "base_url": "rtsp://192.168.0.100:8554",
    "path_pattern": "/screen{n}"
  }
}
```

`{n}` is replaced with the monitor number (1, 2, 3, …). The primary monitor also receives an alias at `/screen0`.

## Project Layout

```text
screen-relay/
├── src/
│   ├── app/          # MonitorSupervisor + ScreenPipeline + StateMachine
│   ├── capture/      # ICaptureBackend, DxgiCaptureBackend, FramePump
│   ├── common/       # Types, error codes, time utilities
│   ├── config/       # JSON config loader
│   ├── encoder/      # FFmpeg H.264 encoder (NVENC / h264_mf / libx264)
│   ├── logging/      # spdlog JSON Lines sink
│   ├── metrics/      # MetricsStore → health.json / metrics.json
│   ├── monitor/      # MonitorDetector (EnumDisplayMonitors)
│   └── rtsp/         # FFmpeg RTSP ANNOUNCE/RECORD client
├── tests/
│   └── unit/         # Custom-runner unit tests
├── config/
│   ├── config.example.json
│   └── config.json   # (gitignored, your local config)
└── docs/             # Category-specific documentation (Japanese)
```

## Documentation

| Doc | Contents |
|-----|----------|
| [docs/architecture.md](docs/architecture.md) | System architecture, data flow, thread model |
| [docs/build.md](docs/build.md) | Detailed build instructions and dependency setup |
| [docs/configuration.md](docs/configuration.md) | Full configuration reference |
| [docs/state-machine.md](docs/state-machine.md) | State machine: states, transitions, error handling |
| [docs/metrics.md](docs/metrics.md) | Metrics/health JSON format, event log format |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Common errors and solutions |

> All docs are written in Japanese. See [README-ja.md](README-ja.md) for the Japanese README.

## Running Tests

```powershell
build/tests/screen_relay_tests.exe
# Expected output ends with: All tests passed.
```

## License

The project is licensed under the [MIT License](LICENSE).

This project uses:

- [FFmpeg](https://ffmpeg.org/) — LGPL-2.1+ (win64-lgpl-shared build)
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
- [winpthread (mingw-w64)](https://sourceforge.net/projects/mingw-w64/) — BSD 2-Clause

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full license texts.

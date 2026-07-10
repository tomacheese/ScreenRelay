# CLAUDE.md

## Project overview

screen-relay is a Windows-only C++17 application that captures every connected
monitor via the DXGI Desktop Duplication API, encodes each display as H.264 with
FFmpeg, and publishes them as individual RTSP streams to a server such as
MediaMTX. Each monitor maps to `/screen{n}`; the primary monitor also gets a
`/screen0` alias. Encoding prefers NVIDIA NVENC and falls back to `h264_mf`
(Media Foundation) then `libx264`. The pipeline is state-machine driven with
exponential back-off reconnection on capture/encoder/RTSP failures.

## Build & test commands

The project builds on Windows with MinGW-w64 GCC (Ninja generator). It cannot be
built on Linux — it links against `d3d11`, `dxgi`, and other Win32 libraries.

- Configure: `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DBUILD_TESTS=ON`
- Build: `cmake --build build`
- Run tests: `build/tests/screen_relay_tests.exe` (successful runs end with `All tests passed.`)
- Run app: `build/screen-relay.exe --config config/config.json`

FFmpeg is resolved via pkg-config first (MSYS2: `pacman -S mingw-w64-x86_64-ffmpeg`),
otherwise from `deps/ffmpeg/` (LGPL shared build from BtbN, avcodec ≥ 62).
spdlog (v1.15.1) and nlohmann/json (v3.12.0) are fetched automatically via CMake
`FetchContent`.

There is no linter or formatter configured (no `.clang-format` / `.clang-tidy`).
CI runs build + unit tests only.

## Architecture

- `src/main.cpp` — entry point, argv parsing, Win32 console-ctrl handler for
  graceful shutdown, launches `MonitorSupervisor`.
- `src/app/` — `MonitorSupervisor`, per-monitor `ScreenPipeline`, and the
  `StateMachine` driving pipeline transitions.
- `src/capture/` — `ICaptureBackend` interface, `DxgiCaptureBackend`, `FramePump`.
- `src/monitor/` — `MonitorDetector` (`EnumDisplayMonitors`); handles runtime
  connect/disconnect and resolution changes without restart.
- `src/encoder/` — `EncoderController`: FFmpeg H.264 with NVENC → h264_mf →
  libx264 fallback.
- `src/rtsp/` — FFmpeg RTSP ANNOUNCE/RECORD publisher client.
- `src/metrics/` — `MetricsStore` writes `health.json` / `metrics.json` to disk.
- `src/logging/` — spdlog JSON Lines event sink.
- `src/config/` — JSON config loader (see `config/config.example.json`).
- `src/common/` — shared `types.hpp`, `errors.hpp`, `time_utils.hpp`.

The application source lives in the static library `screen_relay_lib`; both the
executable and the test runner link against it.

## Testing approach

Unit tests use a custom runner, not a third-party framework. `tests/unit/test_main.cpp`
calls each module's `run_*_tests()` function in sequence; assertions use the
`VERIFY` / `VERIFY_MSG` macros in `tests/unit/test_utils.hpp`, which call
`std::exit(1)` on failure (active regardless of `NDEBUG`). To add a test module,
add its source to `tests/CMakeLists.txt` and call its `run_*_tests()` from
`test_main.cpp`. Tests cover config loading, state machine, back-off, metrics,
and monitor detection; capture/encoder/RTSP paths require real Windows hardware
and are not unit-tested.

## Coding conventions

- C++17. Comments and Doxygen blocks are written in Japanese; log messages,
  event names, and user-facing error strings are in English.
- Keep new documentation under `docs/` in Japanese to match the existing set.
- Prefer the existing lightweight namespace style (`errors`, `time_utils`);
  most types are declared without a global project namespace.

## Configuration & runtime

- `config/config.example.json` is the annotated template; `config/config.json`
  is gitignored (local-only, holds real values).
- Runtime output (`logs/`, `state/`) and downloaded binary deps
  (`deps/ffmpeg/`, `deps/mediamtx/`) are gitignored — never commit them.

## Documentation update rules

- Changing build steps, dependencies, or the run command → update `README.md`,
  `README-ja.md`, and this file.
- Changing the config schema → update `config/config.example.json` and
  `docs/configuration.md`.
- Changing pipeline states/transitions → update `docs/state-machine.md`.
- Changing the metrics/health/event-log format → update `docs/metrics.md`.

## Repository conventions

- Commits follow Conventional Commits; descriptions are written in Japanese
  (matching existing history). CI derives the release version and changelog from
  commit types (`feat` → minor, `fix` → patch, etc.).
- CI (`.github/workflows/ci.yml`) builds on `windows-latest` via MSYS2/MINGW64,
  runs unit tests, packages the binary with FFmpeg LGPL DLLs, and publishes a
  GitHub Release on merge to `master`.
- Dependency updates are managed by Renovate (config extended from
  `book000/templates`).

## Security & prohibitions

- Never commit `config/config.json` or any file containing real RTSP hosts /
  credentials.
- Do not commit downloaded binaries (`deps/ffmpeg/`, FFmpeg DLLs, MediaMTX).
- FFmpeg is used under LGPL via shared libraries — keep it dynamically linked and
  preserve `THIRD_PARTY_LICENSES.md` in release packages.

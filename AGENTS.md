# AGENTS.md

## Language

- All deliverables are written in **English**: commit messages, PR/Issue titles and bodies, review comments, code comments and Doxygen blocks, documentation under `docs/`, and `README.md`.
- Log messages, event names, and error strings are English as well.
- Exceptions: `README-ja.md` (the Japanese translation of `README.md`; keep it in sync when `README.md` changes), quotations, technical identifiers, generated files, and external source text.
- Do not rewrite existing Git history to change its language.

## Project overview

screen-relay is a Windows-only C++17 application that captures every connected monitor via the DXGI Desktop Duplication API, encodes each display as H.264 with FFmpeg, and publishes each one as an RTSP stream to a server such as MediaMTX.
Each monitor maps to `/screen{n}`; the primary monitor also gets a `/screen0` alias. Note that the built-in default `rtsp.path_pattern` is `/live/screen{n}` (`src/common/types.hpp`), while `config/config.example.json` uses `/screen{n}`.
Encoding prefers NVIDIA NVENC and falls back to `h264_mf` (Media Foundation), then `libx264`.
Optional shared WASAPI audio (AAC) is added to every stream.
Each monitor runs a state-machine-driven pipeline with exponential back-off reconnection on capture, encoder, and RTSP failures.

## Build and test

The project builds on Windows with MinGW-w64 GCC and Ninja. It cannot be built on Linux because it links against `d3d11`, `dxgi`, and other Win32 libraries.

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DBUILD_TESTS=ON
cmake --build build
./build/tests/screen_relay_tests.exe        # ends with "All tests passed."; FFmpeg DLLs must be on PATH
./build/screen-relay.exe --config config/config.json
```

- FFmpeg is resolved via pkg-config first (MSYS2: `pacman -S mingw-w64-x86_64-ffmpeg`), otherwise from `deps/ffmpeg/` (LGPL shared build from BtbN, avcodec >= 62).
- spdlog (v1.15.1) and nlohmann/json (v3.12.0) are fetched by CMake `FetchContent`.
- No linter or formatter is configured (no `.clang-format` / `.clang-tidy`). CI builds and runs the unit tests only.
- Details: `docs/build.md`.

## Architecture

- `src/main.cpp`: entry point, argv parsing, Win32 console-ctrl handler for graceful shutdown, starts `MonitorSupervisor`.
- `src/app/`: `MonitorSupervisor`, per-monitor `ScreenPipeline`, and the `StateMachine` driving pipeline transitions.
- `src/capture/`: `ICaptureBackend`, `DxgiCaptureBackend`, `FramePump`.
- `src/monitor/`: `MonitorDetector` (`EnumDisplayMonitors`); runtime connect/disconnect and resolution changes are handled without restart.
- `src/encoder/`: `EncoderController` (FFmpeg H.264, NVENC → h264_mf → libx264, optional GPU zero-copy path).
- `src/rtsp/`: FFmpeg RTSP ANNOUNCE/RECORD publisher client.
- `src/audio/`: WASAPI capture, AAC encoder, and the shared `AudioPipeline`.
- `src/metrics/`: `MetricsStore` writes `health.json` / `metrics.json`.
- `src/logging/`: spdlog JSON Lines event sink.
- `src/config/`: JSON config loader (template: `config/config.example.json`).
- `src/common/`: shared `types.hpp`, `errors.hpp`, `time_utils.hpp`.

The application source is the static library `screen_relay_lib`; both the executable and the test runner link against it. See `docs/architecture.md` and `docs/state-machine.md`.

## Testing

Unit tests use a custom runner, not a third-party framework. `tests/unit/test_main.cpp` calls each module's `run_*_tests()` in sequence, and assertions use the `VERIFY` / `VERIFY_MSG` macros in `tests/unit/test_utils.hpp`, which call `std::exit(1)` on failure (active regardless of `NDEBUG`).
To add a test module, add its source to `tests/CMakeLists.txt` and call its `run_*_tests()` from `test_main.cpp`.
Tests cover config loading, the state machine, back-off, metrics, monitor detection, and the audio pipeline. Capture, encoder, and RTSP paths need real Windows hardware and are not unit-tested.

## Conventions

- C++17. Prefer the existing lightweight namespace style (`errors`, `time_utils`); most types are declared without a project-wide namespace.
- Comments explain intent and constraints, not what the code already says.
- Failures are reported through return values / error strings and state transitions, not exceptions.
- Commits follow Conventional Commits. CI derives the release version and changelog from commit types (`feat` → minor, `fix`/`docs`/`chore`/... → patch).
- CI (`.github/workflows/ci.yml`) builds on `windows-latest` via MSYS2/MINGW64, runs the unit tests, packages the binary with the FFmpeg LGPL DLLs, and publishes a GitHub Release when a PR is merged to `master`.
- Dependency updates are handled by Renovate.

## Keep docs in sync

- Build steps, dependencies, or the run command: `README.md`, `README-ja.md`, `docs/build.md`, and this file.
- Config schema: `config/config.example.json`, `src/config/config_loader.cpp`, and `docs/configuration.md`.
- Pipeline states or transitions: `docs/state-machine.md`.
- Metrics, health, or event-log format: `docs/metrics.md`.

## Security and prohibitions

- Never commit `config/config.json` (gitignored; holds real values) or any file containing real RTSP hosts or credentials.
- Never commit downloaded binaries (`deps/ffmpeg/`, FFmpeg DLLs, `deps/mediamtx/`) or runtime output (`logs/`, `state/`).
- FFmpeg is used under the LGPL as shared libraries. Keep it dynamically linked and keep `THIRD_PARTY_LICENSES.md` in release packages.

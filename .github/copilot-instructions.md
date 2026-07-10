# Copilot code review instructions

screen-relay is a Windows-only C++17 application: DXGI Desktop Duplication
capture → FFmpeg H.264 encode → RTSP publish, one stream per monitor. Review
changes with the priorities below.

## Review priorities

- **Resource lifetime.** DXGI/D3D11 COM objects, FFmpeg contexts
  (`AVCodecContext`, `AVFormatContext`, `AVFrame`, `AVPacket`), and OS handles
  must be released on every path, including error and early-return paths. Flag
  leaks and missing cleanup on failure.
- **Error handling over exceptions.** The codebase reports failures through
  return codes / error strings and state transitions, not exceptions. Flag new
  code that lets a failure crash the process instead of driving the state
  machine into a recoverable/back-off state.
- **Reconnection & back-off.** Capture, encoder, and RTSP failures must remain
  self-healing via exponential back-off. Flag changes that can busy-loop, retry
  with no delay, or leave a pipeline stuck.
- **Thread safety.** The supervisor runs per-monitor pipelines concurrently.
  Flag shared state accessed without synchronization; note that `std::atomic` is
  the established pattern for stop flags and cross-thread pointers.
- **Runtime monitor changes.** Monitor connect/disconnect and resolution changes
  must be handled without restart. Flag code that assumes a fixed monitor set or
  caches resolution indefinitely.
- **Config compatibility.** If `config/config.example.json` changes, check that
  `src/config/config_loader.cpp` and `docs/configuration.md` stay consistent, and
  that missing optional keys still fall back to sensible defaults.

## Project conventions

- Comments and Doxygen blocks are in Japanese; log messages, event names, and
  error strings are in English. Do not flag Japanese comments as an issue.
- Log events use structured spdlog JSON with an event name plus fields — keep new
  events consistent with the existing pattern.
- New tests must register with the custom runner: add the source to
  `tests/CMakeLists.txt` and call `run_*_tests()` from `tests/unit/test_main.cpp`.
  There is no GoogleTest/Catch2 — do not suggest their macros.
- Commits follow Conventional Commits (descriptions in Japanese); CI derives the
  release version from commit type prefixes.

## Known non-issues (do not flag)

- Absence of `.clang-format` / `.clang-tidy` and the lack of a lint step in CI —
  this is intentional.
- Win32-only APIs (`d3d11`, `dxgi`, `EnumDisplayMonitors`, console-ctrl handler)
  and the lack of cross-platform abstraction — the project targets Windows only.
- The hand-rolled `VERIFY` test macros in `tests/unit/test_utils.hpp` calling
  `std::exit(1)` — this is the intended test-failure mechanism.
- FFmpeg being dynamically linked as LGPL shared libraries — required for license
  compliance; do not suggest static linking.
- Dependency versions pinned via CMake `FetchContent` and Renovate — do not flag
  them as outdated.

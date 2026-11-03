# Copilot code review instructions

Project overview, build/test commands, conventions, and the language rule (English) are in [`AGENTS.md`](../AGENTS.md). Review with the priorities below.

## Review priorities

- **Resource lifetime.** DXGI/D3D11 COM objects, FFmpeg contexts (`AVCodecContext`, `AVFormatContext`, `AVFrame`, `AVPacket`), and OS handles must be released on every path, including error and early-return paths. Flag leaks and missing cleanup on failure.
- **Error handling over exceptions.** Failures are reported through return codes / error strings and state transitions, not exceptions. Flag new code that lets a failure crash the process instead of driving the state machine into a recoverable or back-off state.
- **Reconnection and back-off.** Capture, encoder, and RTSP failures must remain self-healing via exponential back-off. Flag changes that can busy-loop, retry with no delay, or leave a pipeline stuck.
- **Thread safety.** The supervisor runs per-monitor pipelines concurrently. Flag shared state accessed without synchronization; `std::atomic` is the established pattern for stop flags and cross-thread pointers.
- **Runtime monitor changes.** Monitor connect/disconnect, resolution, and primary-monitor changes must be handled without restart. Flag code that assumes a fixed monitor set or caches resolution indefinitely.
- **Config compatibility.** If `config/config.example.json` changes, check that `src/config/config_loader.cpp` and `docs/configuration.md` stay consistent, and that missing optional keys still fall back to sensible defaults.
- **Docs in sync.** Behavior changes to states, metrics format, or build steps must update the matching document listed in `AGENTS.md`.

## Project conventions

- Code comments, docs, commits, and PR text are in English (`README-ja.md` is the only Japanese document). Flag newly added non-English comments or messages.
- Log events use structured spdlog JSON with an event name plus fields. Keep new events consistent with the existing pattern.
- New tests must register with the custom runner: add the source to `tests/CMakeLists.txt` and call `run_*_tests()` from `tests/unit/test_main.cpp`. There is no GoogleTest/Catch2, so do not suggest their macros.
- Commits follow Conventional Commits; CI derives the release version from the type prefix.

## Known non-issues (do not flag)

- Absence of `.clang-format` / `.clang-tidy` and of a lint step in CI.
- Win32-only APIs (`d3d11`, `dxgi`, `EnumDisplayMonitors`, console-ctrl handler) and the lack of cross-platform abstraction. The project targets Windows only.
- The hand-rolled `VERIFY` macros in `tests/unit/test_utils.hpp` calling `std::exit(1)`.
- FFmpeg being dynamically linked as LGPL shared libraries. This is required for license compliance; do not suggest static linking.
- Dependency versions pinned via CMake `FetchContent` and Renovate.

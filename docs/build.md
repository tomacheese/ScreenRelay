# Build instructions

screen-relay builds on Windows with MinGW-w64 GCC. It links against Win32 libraries (`d3d11`, `dxgi`, …) and cannot be built on Linux or macOS.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| GCC (MinGW-w64) | 13 or newer recommended | Check with `gcc --version` |
| CMake | 3.20 or newer | Check with `cmake --version` |
| Ninja | Any recent version | Check with `ninja --version` |
| FFmpeg | LGPL shared build (avcodec ≥ 62) | See below |

> **Note**: MSVC is not supported. Use the MinGW-w64 toolchain.

spdlog (v1.15.1) and nlohmann/json (v3.12.0) are downloaded automatically by CMake `FetchContent` during configuration, so the first configure needs network access.

---

## Setting up FFmpeg

CMake looks for FFmpeg in this order:

1. pkg-config (`libavcodec libavformat libavutil libswscale libswresample`). On MSYS2: `pacman -S mingw-w64-x86_64-ffmpeg`.
2. `deps/ffmpeg/` (or the path given by `-DFFMPEG_ROOT=<path>`).

For option 2, place an LGPL shared build of FFmpeg in `deps/ffmpeg/`. Prebuilt binaries are available from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases): download a `*-win64-lgpl-shared*.zip` and extract its contents (`bin/`, `include/`, `lib/`) into `deps/ffmpeg/`.

```text
deps/
└── ffmpeg/
    ├── bin/          # *.dll (needed at run time)
    ├── include/
    │   ├── libavcodec/
    │   ├── libavformat/
    │   ├── libavutil/
    │   ├── libswscale/
    │   └── libswresample/
    └── lib/
        ├── libavcodec.dll.a
        ├── libavformat.dll.a
        ├── libavutil.dll.a
        ├── libswscale.dll.a
        └── libswresample.dll.a
```

`deps/ffmpeg/` is gitignored; do not commit it.

---

## Configure

```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON
```

For a debug build, use `-DCMAKE_BUILD_TYPE=Debug`. `BUILD_TESTS` defaults to `ON`.

`toolchain-mingw.cmake` expects the MinGW compilers in `/mingw64/bin` (the MSYS2 MINGW64 default). Override with `-DMINGW_BIN=<path>` (and `-DMINGW_ROOT=<path>` for the find root), or with the `MINGW_BIN` / `MINGW_ROOT` environment variables.

---

## Build

```bash
# All targets
cmake --build build

# Main binary only
cmake --build build --target screen-relay

# Test binary only
cmake --build build --target screen_relay_tests
```

The outputs are:

```text
build/
├── screen-relay.exe
└── tests/
    └── screen_relay_tests.exe
```

---

## FFmpeg DLLs

FFmpeg's DLLs must be findable at run time. Copy them next to the executable:

```bash
# Git Bash / MSYS2
cp deps/ffmpeg/bin/*.dll build/
```

When FFmpeg comes from MSYS2 (pkg-config), the DLLs are already in `/mingw64/bin`. Release packages (built by CI) also bundle `libwinpthread-1.dll` and `THIRD_PARTY_LICENSES.md`.

---

## Running the tests

The test runner is a plain executable (no framework). It needs the FFmpeg DLLs on `PATH`:

```bash
export PATH="$PWD/deps/ffmpeg/bin:$PATH"
./build/tests/screen_relay_tests.exe
```

A successful run ends with `All tests passed.`. The same executable is also registered with CTest from the `tests/` directory:

```bash
ctest --test-dir build/tests --output-on-failure
```

---

## CMake targets

| Target | Type | Description |
|---|---|---|
| `screen_relay_lib` | Static library | Core logic, shared by `screen-relay` and `screen_relay_tests` |
| `screen-relay` | Executable | The application (`screen-relay.exe`) |
| `screen_relay_tests` | Executable | Unit tests (`screen_relay_tests.exe`) |

---

## Troubleshooting

### CMake cannot find FFmpeg

The configure step fails with `FFmpeg not found`. Install FFmpeg through MSYS2, or check that `deps/ffmpeg/include` exists (or pass `-DFFMPEG_ROOT=<path>`).

### `libavcodec.dll.a` is missing

Check that `deps/ffmpeg/lib` is populated. Import libraries (`.dll.a`) are only part of the shared builds; static builds use different file names.

### `cc1: error: unrecognized command-line option`

Your MinGW-w64 GCC is probably too old. Use GCC 13 or newer.

### Ninja is not found

```bash
# With Scoop
scoop install ninja

# Or use the MinGW Makefiles generator instead
cmake -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
```

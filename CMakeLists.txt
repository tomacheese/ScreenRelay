cmake_minimum_required(VERSION 3.20)
project(ScreenRelay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)

# FFmpeg (pkg-config 優先、なければ deps/ffmpeg/ を使用)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET
        libavcodec libavformat libavutil libswscale libswresample)
endif()

if(NOT FFMPEG_FOUND)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/deps/ffmpeg" CACHE PATH "FFmpeg root")
    if(NOT EXISTS "${FFMPEG_ROOT}/include")
        message(FATAL_ERROR
            "FFmpeg not found.\n"
            "  MSYS2: pacman -S mingw-w64-x86_64-ffmpeg\n"
            "  Manual: set -DFFMPEG_ROOT=<path>")
    endif()
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES
        "${FFMPEG_ROOT}/lib/libavcodec.dll.a"
        "${FFMPEG_ROOT}/lib/libavformat.dll.a"
        "${FFMPEG_ROOT}/lib/libavutil.dll.a"
        "${FFMPEG_ROOT}/lib/libswscale.dll.a"
        "${FFMPEG_ROOT}/lib/libswresample.dll.a"
    )
    set(FFMPEG_FOUND TRUE)
endif()

include(FetchContent)

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_TESTS    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

set(LIB_SOURCES
    src/config/config_loader.cpp
    src/app/state_machine.cpp
    src/app/supervisor.cpp
    src/capture/dxgi_capture.cpp
    src/capture/frame_pump.cpp
    src/monitor/monitor_detector.cpp
    src/encoder/encoder_controller.cpp
    src/rtsp/rtsp_publisher_client.cpp
    src/metrics/metrics_store.cpp
    src/logging/log_sink.cpp
)

add_library(screen_relay_lib STATIC ${LIB_SOURCES})

target_include_directories(screen_relay_lib PUBLIC
    src/
    ${FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(screen_relay_lib PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(screen_relay_lib PUBLIC
    ${FFMPEG_LIBRARIES}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    ws2_32
)

if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(screen_relay_lib PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()

add_executable(screen-relay src/main.cpp)
target_link_libraries(screen-relay PRIVATE screen_relay_lib)

if(MINGW)
    target_link_options(screen-relay PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,--whole-archive,-lwinpthread,--no-whole-archive,-Bdynamic
    )
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

#pragma once
#include <string>

/** Error code constants, used as the "code" field in logs and metrics. */
namespace errors {
    constexpr const char* CAPTURE_INIT_FAILED   = "CAPTURE_INIT_FAILED";
    constexpr const char* ENCODER_INIT_FAILED   = "ENCODER_INIT_FAILED";
    constexpr const char* ENCODER_ENCODE_FAILED = "ENCODER_ENCODE_FAILED";
    constexpr const char* RTSP_CONNECT_FAILED   = "RTSP_CONNECT_FAILED";
    constexpr const char* RTSP_SEND_FAILED      = "RTSP_SEND_FAILED";
    constexpr const char* RTSP_TIMEOUT          = "RTSP_TIMEOUT";
    constexpr const char* MONITOR_ENUM_FAILED   = "MONITOR_ENUM_FAILED";
    constexpr const char* GPU_CRASH             = "GPU_CRASH";
} // namespace errors

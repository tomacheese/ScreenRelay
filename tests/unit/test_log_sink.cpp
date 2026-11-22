#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "common/url_utils.hpp"
#include "logging/log_sink.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;

/**
 * @brief Tests URL redaction and its use by LogSink.
 * @return 0 on success (a failing VERIFY exits with status 1)
 */
int run_log_sink_tests() {
    printf("=== LogSink Tests ===\n");

    {
        const std::pair<const char*, const char*> cases[] = {
            {"rtsp://host:8554/screen1",            "rtsp://host:8554/screen1"},
            {"rtsp://user:pass@host:8554/screen1",  "rtsp://***@host:8554/screen1"},
            {"rtsp://user:p@ss@host/screen1",       "rtsp://***@host/screen1"},
            {"rtsp://user@host/a?pass=x&k=v#f",     "rtsp://***@host/a?***"},
            {"not a url",                           "not a url"},
            {"rtsp://h/a",                          "rtsp://h/a"},
            {"rtsp://h/a?k=v",                      "rtsp://h/a?***"},
            {"rtsp://h/a#f",                        "rtsp://h/a"},
            {"rtsp://h/a#f?x=1",                    "rtsp://h/a"},
            {"rtsp://u:p@h",                        "rtsp://***@h"},
            {"",                                    ""},
        };
        for (const auto& c : cases) {
            const std::string got = url_utils::redact_url(c.first);
            VERIFY_MSG(got == c.second, c.first);
        }
        printf("[PASS] redact_url masks userinfo, query, and fragment\n");
    }

    {
        const fs::path dir = fs::temp_directory_path() / "screen_relay_log_sink_test_tmp";
        fs::remove_all(dir);
        {
            LogSink sink;
            std::string err;
            VERIFY_MSG(sink.init(dir.string(), "t", "info", err), err.c_str());
            sink.log_publish_started(1, "rtsp://u:p@h/a", 1920, 1080, 60);
            sink.log_error("C", "m", {{"url", "rtsp://u:p@h/a"}});
            sink.log_error("C", "rtsp://u:p@h/a");
        }

        std::vector<std::string> lines;
        std::ifstream in(dir / "t.jsonl");
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        VERIFY(lines.size() == 3);

        for (int i = 0; i < 2; ++i) {
            VERIFY(lines[i].find("\"url\":\"rtsp://***@h/a\"") != std::string::npos);
            VERIFY(lines[i].find("u:p") == std::string::npos);
        }
        VERIFY(lines[2].find("\"message\":\"rtsp://u:p@h/a\"") != std::string::npos);

        fs::remove_all(dir);
        printf("[PASS] LogSink redacts the url field only\n");
    }

    return 0;
}

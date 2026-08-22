cmake_minimum_required(VERSION 3.20)

add_executable(screen_relay_tests
    unit/test_main.cpp
    unit/test_config_loader.cpp
    unit/test_state_machine.cpp
    unit/test_backoff.cpp
    unit/test_metrics_store.cpp
    unit/test_monitor_detector.cpp
    unit/test_audio_pipeline.cpp
)

target_link_libraries(screen_relay_tests PRIVATE screen_relay_lib)

target_include_directories(screen_relay_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

enable_testing()
add_test(NAME screen_relay_tests COMMAND screen_relay_tests)

cmake_minimum_required(VERSION 3.16)

# ── Google Test (fetched automatically) ──────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# Prevent googletest from overriding our compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ── Unit tests ────────────────────────────────────────────────────────────────

set(UNIT_TEST_SOURCES
    unit/test_rtsp_request.cpp
    unit/test_rtp_packet.cpp
    unit/test_h264_packetizer.cpp
    unit/test_annexb_parser.cpp
    unit/test_blocking_queue.cpp
    unit/test_thread_pool.cpp
)

add_executable(unit_tests ${UNIT_TEST_SOURCES})

target_link_libraries(unit_tests
    PRIVATE
        rtsp_core
        GTest::gtest_main
        GTest::gmock_main
)

gtest_discover_tests(unit_tests)

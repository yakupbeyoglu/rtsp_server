cmake_minimum_required(VERSION 3.16)
project(RTSPServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Per-config optimisation flags
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2"    CACHE STRING "" FORCE)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBAV REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

# ── Optional spdlog backend ───────────────────────────────────────────────────
# Pass -DRTSP_USE_SPDLOG=ON to opt in.  The build will only activate it when
# spdlog is actually found on the system; if not found it falls back silently
# to the built-in logger so the build never breaks.

option(RTSP_USE_SPDLOG "Use spdlog as the logging backend (requires spdlog)" OFF)
option(USE_ASAN "Enable AddressSanitizer" OFF)
option(USE_TSAN "Enable ThreadSanitizer" OFF)

if(USE_ASAN AND USE_TSAN)
    message(FATAL_ERROR "ASan and TSan are mutually exclusive.")
endif()

if(USE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    message(STATUS "ASan: Enabled (Memory leak detection will run with tests)")
endif()

if(USE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
    message(STATUS "TSan: Enabled (Data race detection will run with tests)")
endif()

if(RTSP_USE_SPDLOG)
    find_package(spdlog QUIET)
    if(spdlog_FOUND)
        message(STATUS "spdlog found – using spdlog logging backend")
        set(RTSP_SPDLOG_AVAILABLE ON)
    else()
        # Try pkg-config as a fallback (some distros only ship a .pc file)
        pkg_check_modules(SPDLOG QUIET spdlog)
        if(SPDLOG_FOUND)
            message(STATUS "spdlog found via pkg-config – using spdlog logging backend")
            set(RTSP_SPDLOG_AVAILABLE ON)
        else()
            message(WARNING
                "RTSP_USE_SPDLOG=ON but spdlog was not found. "
                "Falling back to the built-in logger.")
            set(RTSP_SPDLOG_AVAILABLE OFF)
        endif()
    endif()
else()
    set(RTSP_SPDLOG_AVAILABLE OFF)
endif()

# ── Core library (modular, per-module sub-builds) ────────────────────────────

add_subdirectory(src)


add_executable(rtsp-server src/main.cpp)
target_link_libraries(rtsp-server PRIVATE rtsp_core)

install(TARGETS rtsp-server DESTINATION bin)

enable_testing()
add_subdirectory(test)

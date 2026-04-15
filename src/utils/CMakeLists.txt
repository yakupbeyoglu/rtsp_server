add_library(rtspserver_utils STATIC
    StringUtils.cpp
)

target_include_directories(rtspserver_utils
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(rtspserver_utils
    PUBLIC
        Threads::Threads
)

# ── Optional spdlog backend ───────────────────────────────────────────────────
if(RTSP_SPDLOG_AVAILABLE)
    if(spdlog_FOUND)
        target_link_libraries(rtspserver_utils PUBLIC spdlog::spdlog)
    else()
        target_include_directories(rtspserver_utils PUBLIC ${SPDLOG_INCLUDE_DIRS})
        target_link_libraries(rtspserver_utils PUBLIC ${SPDLOG_LIBRARIES})
        target_compile_options(rtspserver_utils PUBLIC ${SPDLOG_CFLAGS_OTHER})
    endif()
    target_compile_definitions(rtspserver_utils PUBLIC RTSP_USE_SPDLOG)
endif()

add_subdirectory(utils)
add_subdirectory(rtsp)
add_subdirectory(sdp)
add_subdirectory(rtp)
add_subdirectory(media)
add_subdirectory(server)

# ── Aggregate library: rtsp_core ─────────────────────────────────────────────
# All downstream targets (executable, tests) link against rtsp_core.
# rtsp_core transitively pulls in all module libraries.
add_library(rtsp_core INTERFACE)

target_link_libraries(rtsp_core
    INTERFACE
        rtspserver_utils
        rtspserver_rtsp
        rtspserver_sdp
        rtspserver_rtp
        rtspserver_media
        rtspserver_server
)

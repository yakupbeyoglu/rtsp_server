add_library(rtspserver_server STATIC
    Reactor.cpp
    RTSPServer.cpp
    RTSPSession.cpp
)

target_include_directories(rtspserver_server
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtspserver_server
    PUBLIC
        rtspserver_utils
        rtspserver_rtsp
        rtspserver_sdp
        rtspserver_rtp
        rtspserver_media
)

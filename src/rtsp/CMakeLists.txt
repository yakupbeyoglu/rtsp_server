add_library(rtspserver_rtsp STATIC
    RTSPRequest.cpp
    RTSPResponse.cpp
)

target_include_directories(rtspserver_rtsp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtspserver_rtsp
    PUBLIC
        rtspserver_utils
)

add_library(rtspserver_rtp STATIC
    RTPPacket.cpp
    H264Packetizer.cpp
    RTPSender.cpp
)

target_include_directories(rtspserver_rtp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtspserver_rtp
    PUBLIC
        rtspserver_utils
)

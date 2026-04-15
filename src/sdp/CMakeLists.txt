add_library(rtspserver_sdp STATIC
    SDPBuilder.cpp
)

target_include_directories(rtspserver_sdp
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rtspserver_sdp
    PUBLIC
        rtspserver_utils
)

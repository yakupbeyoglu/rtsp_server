add_library(rtspserver_media STATIC
    MediaSource.cpp  
)

target_include_directories(rtspserver_media
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${LIBAV_INCLUDE_DIRS}
)

target_link_libraries(rtspserver_media
    PUBLIC
        rtspserver_utils
        rtspserver_sdp
        ${LIBAV_LIBRARIES}
)

target_compile_options(rtspserver_media PUBLIC ${LIBAV_CFLAGS_OTHER})

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtspserver::sdp {

struct H264StreamInfo {
    // Raw SPS / PPS NAL units
    // No start code and length prefix
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    int width { 0 };
    int height { 0 };
    float frame_rate { 25.0f };
    // Total duration in seconds (0 = unknown / live).
    double duration_secs { 0.0 };
};

struct AudioStreamInfo {
    // false present no audito track at all
    bool present { false };
    int sample_rate { 44100 };
    int channels { 2 };
    // MPEG-4 Audio Specific Config (2+ bytes) for AAC.
    std::vector<uint8_t> asc;
};

class SDPBuilder {
public:
    // Build the complete SDP string.
    // `server_ip`   – IP address string that goes in the `o=` and `c=` lines.
    // `stream_name` – human-readable stream name (e.g. filename).
    // `base_url`    – RTSP base URL for `a=control`.
    // `video_pt`    – dynamic payload type for video (default 96).
    // `audio`       – optional audio track info; pass {} for video-only.
    // `audio_pt`    – dynamic payload type for audio (default 97).
    static std::string build(const H264StreamInfo& info,
        const std::string& server_ip,
        const std::string& stream_name,
        const std::string& base_url,
        int video_pt = 96,
        const AudioStreamInfo& audio = {},
        int audio_pt = 97);
};

} // namespace rtspserver::sdp

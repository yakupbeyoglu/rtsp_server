#include "rtspserver/sdp/SDPBuilder.hpp"

#include "rtspserver/utils/StringUtils.hpp"

#include <iomanip>
#include <sstream>

using namespace rtspserver::utils;

namespace rtspserver::sdp {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Format 3 bytes from SPS as uppercase hex for profile-level-id
static std::string profileLevelId(const std::vector<uint8_t>& sps)
{
    // SPS layout (after NAL header byte): profile_idc, constraint_flags, level_idc
    if (sps.size() < 4)
        return "42001f"; // default: Baseline 3.1
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << static_cast<int>(sps[1]) // profile_idc
        << std::setw(2) << static_cast<int>(sps[2]) // constraint flags
        << std::setw(2) << static_cast<int>(sps[3]); // level_idc
    return oss.str();
}

// ── SDPBuilder::build ─────────────────────────────────────────────────────────

std::string SDPBuilder::build(const H264StreamInfo& info,
    const std::string& server_ip,
    const std::string& stream_name,
    const std::string& base_url,
    int video_pt,
    const AudioStreamInfo& audio,
    int audio_pt)
{
    std::ostringstream s;

    // ── Session section ───────────────────────────────────────────────────────
    s << "v=0\r\n";
    s << "o=- 1000 1 IN IP4 " << server_ip << "\r\n";
    s << "s=" << stream_name << "\r\n";
    s << "c=IN IP4 0.0.0.0\r\n";
    s << "t=0 0\r\n";
    // a=range: advertise the file duration so the client can display a seek
    // bar and elapsed time.  For looping playback RTP timestamps are kept
    // monotonically increasing, so on subsequent loops they exceed this bound
    // and the client's progress bar stays at 100 % – but frames are never
    // delivered "late" that way.
    if (info.duration_secs > 0.0) {
        s << "a=range:npt=0-" << std::fixed << std::setprecision(3)
          << info.duration_secs << "\r\n";
    }
    s << "a=recvonly\r\n";
    s << "a=control:" << base_url << "\r\n";
    s << "a=tool:rtsp-server\r\n";

    // ── Media section (H.264 video) ───────────────────────────────────────────
    s << "m=video 0 RTP/AVP " << video_pt << "\r\n";
    s << "a=rtpmap:" << video_pt << " H264/90000\r\n";

    // a=fmtp
    std::string sps_b64 = StringUtils::base64Encode(info.sps);
    std::string pps_b64 = StringUtils::base64Encode(info.pps);
    std::string plid = profileLevelId(info.sps);

    s << "a=fmtp:" << video_pt
      << " packetization-mode=1"
      << ";sprop-parameter-sets=" << sps_b64 << ',' << pps_b64
      << ";profile-level-id=" << plid << "\r\n";

    s << "a=cliprect:0,0," << info.height << "," << info.width << "\r\n";
    s << "a=framesize:" << video_pt << ' ' << info.width << '-' << info.height << "\r\n";
    s << "a=control:trackID=0\r\n";

    // ── Media section (AAC audio) – only when the file has an audio track ─────
    if (audio.present) {
        s << "m=audio 0 RTP/AVP " << audio_pt << "\r\n";
        s << "a=rtpmap:" << audio_pt
          << " MPEG4-GENERIC/" << audio.sample_rate
          << '/' << audio.channels << "\r\n";

        // a=fmtp: RFC 3640 mode=AAC-hbr
        s << "a=fmtp:" << audio_pt
          << " profile-level-id=1"
          << ";mode=AAC-hbr"
          << ";sizelength=13;indexlength=3;indexdeltalength=3";
        if (!audio.asc.empty()) {
            // config= is the hex-encoded MPEG-4 AudioSpecificConfig
            s << ";config=";
            for (uint8_t b : audio.asc) {
                s << std::hex << std::setfill('0') << std::setw(2)
                  << static_cast<int>(b);
            }
            s << std::dec;
        }
        s << "\r\n";
        s << "a=control:trackID=1\r\n";
    }

    return s.str();
}

} // namespace rtspserver::sdp

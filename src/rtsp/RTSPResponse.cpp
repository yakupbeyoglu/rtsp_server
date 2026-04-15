#include "rtspserver/rtsp/RTSPResponse.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace rtspserver::rtsp {

// ── Serialisation ─────────────────────────────────────────────────────────────

std::string RTSPResponse::toString() const
{
    std::ostringstream oss;
    oss << "RTSP/1.0 " << status << ' ' << reason << "\r\n";
    // Always emit a Server header first (improves VLC compatibility)
    oss << "Server: RTSP-Server/1.0\r\n";
    for (auto& [k, v] : headers) {
        oss << k << ": " << v << "\r\n";
    }
    if (!body.empty()) {
        // Content-Length is expected to already be set by the factory; add a
        // safety fallback so we never emit a body without it.
        if (headers.find("Content-Length") == headers.end()) {
            oss << "Content-Length: " << body.size() << "\r\n";
        }
    }
    oss << "\r\n";
    oss << body;
    return oss.str();
}

// ── Timestamp helper ──────────────────────────────────────────────────────────

std::string RTSPResponse::timestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

// ── Factories ─────────────────────────────────────────────────────────────────

RTSPResponse RTSPResponse::options(int cseq, const std::string& public_methods)
{
    RTSPResponse r;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Public"] = public_methods;
    r.headers["Date"] = timestamp();
    return r;
}

RTSPResponse RTSPResponse::describe(int cseq,
    const std::string& sdp,
    const std::string& base_url)
{
    RTSPResponse r;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Date"] = timestamp();
    r.headers["Content-Type"] = "application/sdp";
    r.headers["Content-Base"] = base_url;
    r.headers["Content-Length"] = std::to_string(sdp.size());
    r.body = sdp;
    return r;
}

RTSPResponse RTSPResponse::setup(int cseq,
    const std::string& session_id,
    const std::string& transport)
{
    RTSPResponse r;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Transport"] = transport;
    r.headers["Session"] = session_id + ";timeout=60";
    r.headers["Date"] = timestamp();
    return r;
}

RTSPResponse RTSPResponse::play(int cseq,
    const std::string& session_id,
    const std::string& range,
    const std::string& rtp_info)
{
    RTSPResponse r;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Session"] = session_id;
    r.headers["Date"] = timestamp();
    if (!range.empty())
        r.headers["Range"] = range;
    if (!rtp_info.empty())
        r.headers["RTP-Info"] = rtp_info;
    return r;
}

RTSPResponse RTSPResponse::ok(int cseq, const std::string& session_id)
{
    RTSPResponse r;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Date"] = timestamp();
    if (!session_id.empty())
        r.headers["Session"] = session_id;
    return r;
}

RTSPResponse RTSPResponse::error(int cseq,
    int status_code,
    const std::string& reason_phrase)
{
    RTSPResponse r;
    r.status = status_code;
    r.reason = reason_phrase;
    r.headers["CSeq"] = std::to_string(cseq);
    r.headers["Date"] = timestamp();
    return r;
}

} // namespace rtspserver::rtsp

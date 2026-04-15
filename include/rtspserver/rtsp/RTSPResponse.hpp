#pragma once

#include <map>
#include <string>

namespace rtspserver::rtsp {

/**
 * @brief Builds an RTSP/1.0 response message.
 * RTSP/1.0 <status> <reason>\r\n
 * CSeq: <n>\r\n
 * ...headers...\r\n
 * \r\n
 * [body]
 */
class RTSPResponse {
public:
    int status { 200 };
    std::string reason { "OK" };
    std::map<std::string, std::string> headers;
    std::string body;

    // Serialise to wire format
    std::string toString() const;

    // The command factories
    static RTSPResponse options(int cseq, const std::string& public_methods);
    static RTSPResponse describe(int cseq, const std::string& sdp, const std::string& base_url);
    static RTSPResponse setup(int cseq,
        const std::string& session_id,
        const std::string& transport);
    static RTSPResponse play(int cseq,
        const std::string& session_id,
        const std::string& range = "npt=0.000-",
        const std::string& rtp_info = "");
    static RTSPResponse ok(int cseq, const std::string& session_id = "");
    static RTSPResponse error(int cseq, int status_code, const std::string& reason_phrase);

private:
    static std::string timestamp();
};

} // namespace rtspserver::rtsp

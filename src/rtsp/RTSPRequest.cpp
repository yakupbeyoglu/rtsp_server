#include "rtspserver/rtsp/RTSPRequest.hpp"

#include "rtspserver/utils/StringUtils.hpp"

#include <stdexcept>

using namespace rtspserver::utils;

namespace rtspserver::rtsp {

std::optional<RTSPRequest> RTSPRequest::parse(const std::string& raw)
{
    RTSPRequest req;

    // Split on CRLF lines
    size_t pos = 0;
    auto nextLine = [&]() -> std::optional<std::string> {
        auto crlf = raw.find("\r\n", pos);
        if (crlf == std::string::npos)
            return std::nullopt;
        std::string line = raw.substr(pos, crlf - pos);
        pos = crlf + 2;
        return line;
    };

    // ── Request line ──────────────────────────────────────────────────────────
    auto first = nextLine();
    if (!first)
        return std::nullopt;

    auto parts = StringUtils::split(*first, ' ');
    if (parts.size() < 3)
        return std::nullopt;

    req.method = StringUtils::trim(parts[0]);
    req.url = StringUtils::trim(parts[1]);
    req.version = StringUtils::trim(parts[2]);

    if (req.version.find("RTSP/") != 0)
        return std::nullopt;

    // ── Headers ───────────────────────────────────────────────────────────────
    while (true) {
        auto line = nextLine();
        if (!line)
            break;
        if (line->empty())
            break; // blank line = end of headers

        auto colon = line->find(':');
        if (colon == std::string::npos)
            continue;

        std::string name = StringUtils::toLower(StringUtils::trim(line->substr(0, colon)));
        std::string value = StringUtils::trim(line->substr(colon + 1));
        req.headers[name] = value;
    }

    // ── Body (based on Content-Length) ────────────────────────────────────────
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        try {
            size_t body_len = static_cast<size_t>(std::stoul(it->second));
            if (pos + body_len <= raw.size()) {
                req.body = raw.substr(pos, body_len);
            }
        } catch (...) {
        }
    }

    return req;
}

std::string RTSPRequest::header(const std::string& name) const
{
    auto it = headers.find(StringUtils::toLower(name));
    return (it != headers.end()) ? it->second : std::string {};
}

int RTSPRequest::cseq() const
{
    auto val = header("cseq");
    if (val.empty())
        return -1;
    try {
        return std::stoi(val);
    } catch (...) {
        return -1;
    }
}

} // namespace rtspserver::rtsp

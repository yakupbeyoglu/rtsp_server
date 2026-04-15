#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace rtspserver::rtsp {

/**
 * @brief Represents a parsed RTSP request, including method, URL, version, headers, and body.
 *   METHOD url RTSP/1.0\r\n
 *   Header: value\r\n
 *   \r\n
 *   [body]
 */
struct RTSPRequest {
    std::string method;
    std::string url;
    std::string version;

    // Header names stored lower-case so look-ups are case-insensitive.
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // Parse a complete RTSP request from a raw string.
    // Returns std::nullopt when the message is malformed.
    [[nodiscard]] static std::optional<RTSPRequest> parse(const std::string& raw);

    // Returns the header value (case-insensitive lookup), or "" if absent.
    std::string header(const std::string& name) const;

    // Returns the CSeq value, or -1 if missing/invalid.
    int cseq() const;
};

} // namespace rtspserver::rtsp

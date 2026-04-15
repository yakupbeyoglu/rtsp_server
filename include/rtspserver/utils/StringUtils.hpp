#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtspserver::utils::StringUtils {

std::string trim(std::string_view s);
std::string toLower(std::string_view s);
std::vector<std::string> split(std::string_view s, char delimiter);
std::string base64Encode(std::span<const uint8_t> data);
bool parseHostPort(const std::string& hostport, std::string& host, uint16_t& port);
std::string urlPath(const std::string& url);
std::string stripTrackSuffix(const std::string& path);
std::string urlDecode(const std::string& s);

} // namespace rtspserver::utils::StringUtils

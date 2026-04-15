#include "rtspserver/utils/StringUtils.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace rtspserver::utils::StringUtils {

std::string trim(std::string_view s)
{
    auto first = s.begin();
    auto last = s.end();

    while (first != last && std::isspace(static_cast<unsigned char>(*first)))
        ++first;

    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1))))
        --last;

    return std::string(first, last);
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::vector<std::string> split(std::string_view s, char delimiter)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t pos;

    while ((pos = s.find(delimiter, start)) != std::string_view::npos) {
        tokens.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    tokens.emplace_back(s.substr(start));
    return tokens;
}

static constexpr char kB64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::span<const uint8_t> data)
{
    const uint8_t* d = data.data();
    const size_t len = data.size();
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(d[i]) << 16;
        if (i + 1 < len)
            b |= static_cast<uint32_t>(d[i + 1]) << 8;
        if (i + 2 < len)
            b |= static_cast<uint32_t>(d[i + 2]);

        out += kB64Table[(b >> 18) & 0x3Fu];
        out += kB64Table[(b >> 12) & 0x3Fu];
        out += (i + 1 < len) ? kB64Table[(b >> 6) & 0x3Fu] : '=';
        out += (i + 2 < len) ? kB64Table[b & 0x3Fu] : '=';
    }
    return out;
}

bool parseHostPort(const std::string& hostport,
    std::string& host,
    uint16_t& port)
{
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos)
        return false;

    host = hostport.substr(0, colon);
    try {
        int p = std::stoi(hostport.substr(colon + 1));
        if (p < 1 || p > 65535)
            return false;
        port = static_cast<uint16_t>(p);
    } catch (...) {
        return false;
    }
    return !host.empty();
}

std::string urlPath(const std::string& url)
{
    // Find "//", then skip host:port, then return from first '/' onward
    auto dslash = url.find("//");
    if (dslash == std::string::npos)
        return url;

    auto slash = url.find('/', dslash + 2);
    if (slash == std::string::npos)
        return "/";
    return url.substr(slash);
}

std::string stripTrackSuffix(const std::string& path)
{
    auto slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return path;

    std::string segment = toLower(path.substr(slash + 1));
    if (segment.find("trackid") != std::string::npos || segment.find("track") != std::string::npos) {
        return path.substr(0, slash);
    }
    return path;
}

std::string urlDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

} // namespace StringUtils

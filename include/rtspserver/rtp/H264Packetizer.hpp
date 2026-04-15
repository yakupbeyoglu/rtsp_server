#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rtspserver::rtp {

/**
 * @brief H.264 to RTP packetizer (RFC 6184)
 * Hanlde two packetization modes:
 *  Single NAL unit packet  – when the NAL fits within the MTU
 *  FU-A fragmentation      – when the NAL exceeds the MTU
 */
class H264Packetizer {
public:
    struct Packet {
        // rtp payload (header not included here)
        std::vector<uint8_t> payload; // RTP payload (header not included here)
        // mbit on this packet (set on the last packet of a frame, or the only packet if it fits in one)
        bool marker;
    };

    // Packetize a single NAL unit into one or more RTP payloads.
    // `nal` – view of the NAL data (first byte is the NAL header)
    // `mtu` – maximum transmission unit for the RTP payload (default 1400)
    /**
     * @brief Packetize a single NAL unit into one or more RTP payloads.
     * The input `nal` is a view of the NAL data, where the first byte is the NAL header.
     * The `mtu` parameter specifies the maximum transmission unit for the RTP payload, with a default value of 1400 bytes.
     *
     * @param nal
     * @param mtu
     * @return std::vector<Packet>
     */
    std::vector<Packet> packetize(std::span<const uint8_t> nal,
        size_t mtu = 1400) const;
};

} // namespace rtspserver::rtp

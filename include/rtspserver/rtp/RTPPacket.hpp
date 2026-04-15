#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace rtspserver::rtp {

class RTPPacket {
public:
    // Build a complete RTP packet (header + payload) into a byte vector.
    // `payload`      – view of RTP payload bytes
    // `payload_type` – 7-bit PT field (dynamic, e.g. 96)
    // `seq`          – sequence number
    // `timestamp`    – media timestamp
    // `ssrc`         – synchronisation source identifier
    // `marker`       – set the M bit (last packet of an access unit)
    static std::vector<uint8_t> build(std::span<const uint8_t> payload,
        uint8_t payload_type,
        uint16_t seq,
        uint32_t timestamp,
        uint32_t ssrc,
        bool marker);

private:
    static constexpr int kHeaderSize = 12;
};

} // namespace rtspserver::rtp

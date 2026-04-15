#include "rtspserver/rtp/RTPPacket.hpp"

namespace rtspserver::rtp {

std::vector<uint8_t> RTPPacket::build(std::span<const uint8_t> payload,
    uint8_t payload_type,
    uint16_t seq,
    uint32_t timestamp,
    uint32_t ssrc,
    bool marker)
{
    std::vector<uint8_t> pkt(kHeaderSize + payload.size());

    // Byte 0: V=2, P=0, X=0, CC=0
    pkt[0] = 0x80u;
    // Byte 1: M | PT
    pkt[1] = static_cast<uint8_t>((marker ? 0x80u : 0x00u) | (payload_type & 0x7Fu));
    // Bytes 2-3: sequence number (big-endian)
    pkt[2] = static_cast<uint8_t>(seq >> 8);
    pkt[3] = static_cast<uint8_t>(seq);
    // Bytes 4-7: timestamp (big-endian)
    pkt[4] = static_cast<uint8_t>(timestamp >> 24);
    pkt[5] = static_cast<uint8_t>(timestamp >> 16);
    pkt[6] = static_cast<uint8_t>(timestamp >> 8);
    pkt[7] = static_cast<uint8_t>(timestamp);
    // Bytes 8-11: SSRC (big-endian)
    pkt[8] = static_cast<uint8_t>(ssrc >> 24);
    pkt[9] = static_cast<uint8_t>(ssrc >> 16);
    pkt[10] = static_cast<uint8_t>(ssrc >> 8);
    pkt[11] = static_cast<uint8_t>(ssrc);

    if (!payload.empty()) {
        std::memcpy(pkt.data() + kHeaderSize, payload.data(), payload.size());
    }
    return pkt;
}

} // namespace rtspserver::rtp

#include "rtspserver/rtp/H264Packetizer.hpp"

#include <algorithm>

namespace rtspserver::rtp {

std::vector<H264Packetizer::Packet>
H264Packetizer::packetize(std::span<const uint8_t> nal,
    size_t mtu) const
{
    if (nal.empty())
        return {};

    std::vector<Packet> packets;

    if (nal.size() <= mtu) {
        // Single nall unit
        Packet p;
        p.payload.assign(nal.begin(), nal.end());
        p.marker = true;
        packets.push_back(std::move(p));
        return packets;
    }

    // Fragmented NAL unit using FU-A (RFC 6184 §5.8)
    const uint8_t nal_header = nal[0];
    const uint8_t nal_type = nal_header & 0x1Fu;
    const uint8_t nri = nal_header & 0x60u;
    const uint8_t fu_indicator = nri | 28u; // type 28 = FU-A

    // Skip the NAL header byte; it is encoded in the FU header instead
    auto data = nal.subspan(1);
    bool first = true;
    const size_t fragment_size = mtu - 2u; // 1 FU-indicator + 1 FU-header

    while (!data.empty()) {
        size_t chunk = std::min(data.size(), fragment_size);
        bool last = (chunk == data.size());

        uint8_t fu_header = nal_type;
        if (first)
            fu_header |= 0x80u; // S bit
        if (last)
            fu_header |= 0x40u; // E bit

        Packet p;
        p.payload.reserve(2 + chunk);
        p.payload.push_back(fu_indicator);
        p.payload.push_back(fu_header);
        p.payload.insert(p.payload.end(), data.begin(), data.begin() + chunk);
        p.marker = last;

        packets.push_back(std::move(p));
        data = data.subspan(chunk);
        first = false;
    }

    return packets;
}

} // namespace rtspserver::rtp

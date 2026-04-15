#include <gtest/gtest.h>

#include "rtspserver/rtp/H264Packetizer.hpp"

#include <numeric> // std::iota

using namespace rtspserver::rtp;

static std::vector<uint8_t> makeNAL(size_t size, uint8_t nal_type = 5 /*IDR*/)
{
    std::vector<uint8_t> nal(size);
    nal[0] = 0x60u | nal_type; // NRI=3, type=nal_type
    std::iota(nal.begin() + 1, nal.end(), static_cast<uint8_t>(1));
    return nal;
}

TEST(H264PacketizerTest, SmallNALProducesSinglePacket)
{
    H264Packetizer pac;
    auto nal = makeNAL(100);
    auto packets = pac.packetize(std::span { nal });

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].payload, nal);
    EXPECT_TRUE(packets[0].marker);
}

TEST(H264PacketizerTest, SinglePacketAtExactMTU)
{
    H264Packetizer pac;
    const size_t mtu = 1400;
    auto nal = makeNAL(mtu);
    auto packets = pac.packetize(std::span { nal }, mtu);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker);
}

TEST(H264PacketizerTest, LargeNALFragments)
{
    H264Packetizer pac;
    const size_t mtu = 1400;
    const size_t nal_size = 4200; // needs 3 fragments
    auto nal = makeNAL(nal_size, 1 /*P-slice*/);
    auto packets = pac.packetize(std::span { nal }, mtu);

    // Number of fragments = ceil((nal_size - 1) / (mtu - 2))
    size_t expected = (nal_size - 1 + (mtu - 2) - 1) / (mtu - 2);
    ASSERT_EQ(packets.size(), expected);
}

TEST(H264PacketizerTest, FUAHeaderStructure)
{
    H264Packetizer pac;
    const size_t mtu = 100;
    auto nal = makeNAL(500, 5 /*IDR*/);
    auto packets = pac.packetize(std::span { nal }, mtu);

    ASSERT_GE(packets.size(), 2u);

    uint8_t nal_header = nal[0];
    uint8_t nri = nal_header & 0x60u;
    uint8_t original_type = nal_header & 0x1Fu;

    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& p = packets[i];
        ASSERT_GE(p.payload.size(), 2u);

        // FU indicator: NRI preserved, type = 28 (FU-A)
        EXPECT_EQ(p.payload[0] & 0x60u, nri);
        EXPECT_EQ(p.payload[0] & 0x1Fu, 28u);

        // FU header: correct type
        EXPECT_EQ(p.payload[1] & 0x1Fu, original_type);

        // Start bit only on first fragment
        bool s_bit = (p.payload[1] & 0x80u) != 0;
        EXPECT_EQ(s_bit, i == 0);

        // End bit only on last fragment
        bool e_bit = (p.payload[1] & 0x40u) != 0;
        EXPECT_EQ(e_bit, i == packets.size() - 1);
    }
}

TEST(H264PacketizerTest, OnlyLastPacketHasMarkerBit)
{
    H264Packetizer pac;
    auto nal = makeNAL(5000);
    auto packets = pac.packetize(std::span { nal }, 1400);

    ASSERT_GE(packets.size(), 2u);

    for (size_t i = 0; i + 1 < packets.size(); ++i) {
        EXPECT_FALSE(packets[i].marker) << "fragment " << i << " should not have marker";
    }
    EXPECT_TRUE(packets.back().marker);
}

TEST(H264PacketizerTest, ReassembledDataMatchesOriginal)
{
    H264Packetizer pac;
    const size_t mtu = 50;
    auto nal = makeNAL(300, 1);
    auto packets = pac.packetize(std::span { nal }, mtu);

    // Re-assemble: skip 2-byte FU header, concatenate NAL header + fragments
    std::vector<uint8_t> reassembled;
    // The NAL header byte is encoded in each FU indicator / FU header;
    // reconstruct it from the first fragment's FU indicator and FU header type.
    uint8_t fu_ind = packets[0].payload[0];
    uint8_t fu_hdr = packets[0].payload[1];
    uint8_t nal_header_reconstructed = (fu_ind & 0x60u) | (fu_hdr & 0x1Fu);
    reassembled.push_back(nal_header_reconstructed);

    for (const auto& p : packets) {
        reassembled.insert(reassembled.end(),
            p.payload.begin() + 2,
            p.payload.end());
    }

    EXPECT_EQ(reassembled, nal);
}

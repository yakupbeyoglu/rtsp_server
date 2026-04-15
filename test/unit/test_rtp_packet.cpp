#include <gtest/gtest.h>

#include "rtspserver/rtp/RTPPacket.hpp"

using namespace rtspserver::rtp;

// Convenience: read a big-endian uint16 / uint32 from a byte buffer
static uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
static uint32_t readU32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

TEST(RTPPacketTest, MinimumSize)
{
    auto pkt = RTPPacket::build({}, 96, 0, 0, 0, false);
}

TEST(RTPPacketTest, VersionIsTwo)
{
    auto pkt = RTPPacket::build({}, 96, 1, 0, 0, false);
    // V = bits 7-6 of byte 0
    EXPECT_EQ((pkt[0] >> 6) & 0x03, 2);
}

TEST(RTPPacketTest, PaddingAndExtensionBitsClear)
{
    auto pkt = RTPPacket::build({}, 96, 1, 0, 0, false);
    EXPECT_EQ((pkt[0] >> 5) & 0x01, 0); // P bit
    EXPECT_EQ((pkt[0] >> 4) & 0x01, 0); // X bit
}

TEST(RTPPacketTest, PayloadTypeEncoded)
{
    auto pkt = RTPPacket::build({}, 96, 0, 0, 0, false);
    EXPECT_EQ(pkt[1] & 0x7F, 96);
}

TEST(RTPPacketTest, MarkerBitSet)
{
    auto pkt_m = RTPPacket::build({}, 96, 0, 0, 0, true);
    auto pkt_nm = RTPPacket::build({}, 96, 0, 0, 0, false);
    EXPECT_EQ((pkt_m[1] >> 7) & 0x01, 1);
    EXPECT_EQ((pkt_nm[1] >> 7) & 0x01, 0);
}

TEST(RTPPacketTest, SequenceNumberEncoded)
{
    auto pkt = RTPPacket::build({}, 96, 0xABCD, 0, 0, false);
    EXPECT_EQ(readU16(pkt.data() + 2), 0xABCD);
}

TEST(RTPPacketTest, TimestampEncoded)
{
    auto pkt = RTPPacket::build({}, 96, 0, 0xDEADBEEF, 0, false);
    EXPECT_EQ(readU32(pkt.data() + 4), 0xDEADBEEF);
}

TEST(RTPPacketTest, SSRCEncoded)
{
    auto pkt = RTPPacket::build({}, 96, 0, 0, 0x12345678, false);
    EXPECT_EQ(readU32(pkt.data() + 8), 0x12345678u);
}

TEST(RTPPacketTest, PayloadAppended)
{
    uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04 };
    auto pkt = RTPPacket::build(std::span { payload }, 96, 0, 0, 0, false);
    ASSERT_EQ(pkt.size(), 12u + sizeof(payload));
    EXPECT_EQ(std::memcmp(pkt.data() + 12, payload, sizeof(payload)), 0);
}

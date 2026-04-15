#include <gtest/gtest.h>

#include "rtspserver/media/AnnexBParser.hpp"

using namespace rtspserver::media;

// Build a raw Annex-B stream from a list of NAL payloads using 4-byte start codes.
static std::vector<uint8_t> makeAnnexB(
    const types::NalUnitList& nals,
    bool use_3byte_first = false)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i < nals.size(); ++i) {
        bool three = (i == 0 && use_3byte_first);
        if (!three) {
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x01);
        } else {
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x01);
        }
        out.insert(out.end(), nals[i].begin(), nals[i].end());
    }
    return out;
}

TEST(AnnexBParserTest, StartCode3ByteDetected)
{
    std::vector<uint8_t> sc3 = { 0x00, 0x00, 0x01, 0x05 };
    EXPECT_EQ(AnnexBParser::startCodeLength(sc3.data(), sc3.size(), 0), 3u);
}

TEST(AnnexBParserTest, StartCode4ByteDetected)
{
    std::vector<uint8_t> sc4 = { 0x00, 0x00, 0x00, 0x01, 0x05 };
    EXPECT_EQ(AnnexBParser::startCodeLength(sc4.data(), sc4.size(), 0), 4u);
}

TEST(AnnexBParserTest, StartCodeNotPresent)
{
    std::vector<uint8_t> buf = { 0x00, 0x01, 0x05 };
    EXPECT_EQ(AnnexBParser::startCodeLength(buf.data(), buf.size(), 0), 0u);
}

TEST(AnnexBParserTest, StartCodeAtNonZeroOffset)
{
    std::vector<uint8_t> buf = { 0xAB, 0x00, 0x00, 0x01, 0x41 };
    EXPECT_EQ(AnnexBParser::startCodeLength(buf.data(), buf.size(), 1), 3u);
}

TEST(AnnexBParserTest, StartCodeTruncatedReturnsZero)
{
    std::vector<uint8_t> buf = { 0x00, 0x00 };
    EXPECT_EQ(AnnexBParser::startCodeLength(buf.data(), buf.size(), 0), 0u);
}

TEST(AnnexBParserTest, SplitViewSingleNAL)
{
    std::vector<uint8_t> nal = { 0x67, 0x42, 0xC0, 0x1E };
    auto stream = makeAnnexB({ nal });
    auto result = AnnexBParser::splitView(stream.data(), stream.size());
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result[0].size(), nal.size());
    EXPECT_TRUE(std::equal(result[0].begin(), result[0].end(), nal.begin()));
}

TEST(AnnexBParserTest, SplitViewTwoNALs)
{
    std::vector<uint8_t> sps = { 0x67, 0x42, 0xC0, 0x1E };
    std::vector<uint8_t> pps = { 0x68, 0xCE, 0x38, 0x80 };
    auto stream = makeAnnexB({ sps, pps });
    auto result = AnnexBParser::splitView(stream.data(), stream.size());
    ASSERT_EQ(result.size(), 2u);
    EXPECT_TRUE(std::equal(result[0].begin(), result[0].end(), sps.begin()));
    EXPECT_TRUE(std::equal(result[1].begin(), result[1].end(), pps.begin()));
}

TEST(AnnexBParserTest, SplitViewMixed3And4ByteStartCodes)
{
    std::vector<uint8_t> nal1 = { 0x67, 0x01 };
    std::vector<uint8_t> nal2 = { 0x68, 0x02 };
    // First NAL gets 3-byte SC, second gets 4-byte SC
    auto stream = makeAnnexB({ nal1, nal2 }, /*use_3byte_first=*/true);
    auto result = AnnexBParser::splitView(stream.data(), stream.size());
    ASSERT_EQ(result.size(), 2u);
    EXPECT_TRUE(std::equal(result[0].begin(), result[0].end(), nal1.begin()));
    EXPECT_TRUE(std::equal(result[1].begin(), result[1].end(), nal2.begin()));
}

TEST(AnnexBParserTest, SplitViewEmptyInputReturnsEmpty)
{
    std::vector<uint8_t> empty;
    auto result = AnnexBParser::splitView(empty.data(), empty.size());
    EXPECT_TRUE(result.empty());
}

TEST(AnnexBParserTest, SplitViewNoStartCodeReturnsEmpty)
{
    std::vector<uint8_t> buf = { 0x67, 0x42, 0x01 };
    auto result = AnnexBParser::splitView(buf.data(), buf.size());
    EXPECT_TRUE(result.empty());
}

TEST(AnnexBParserTest, SplitViewSpanOverload)
{
    std::vector<uint8_t> nal = { 0x41, 0x9A };
    auto stream = makeAnnexB({ nal });
    auto result = AnnexBParser::splitView(std::span { stream });
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(std::equal(result[0].begin(), result[0].end(), nal.begin()));
}

TEST(AnnexBParserTest, SplitViewPointsIntoOriginalBuffer)
{
    std::vector<uint8_t> nal = { 0x65, 0xB8, 0x04 };
    auto stream = makeAnnexB({ nal });
    auto result = AnnexBParser::splitView(stream.data(), stream.size());
    ASSERT_EQ(result.size(), 1u);
    // The span must point directly into stream (zero-copy guarantee)
    EXPECT_EQ(result[0].data(), stream.data() + 4);
}

TEST(AnnexBParserTest, SplitOwningSingleNAL)
{
    std::vector<uint8_t> nal = { 0x67, 0x42, 0xC0, 0x1E };
    auto stream = makeAnnexB({ nal });
    auto result = AnnexBParser::split(std::span { stream });
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], nal);
}

TEST(AnnexBParserTest, SplitOwningThreeNALs)
{
    std::vector<uint8_t> sps = { 0x67, 0x42, 0xC0, 0x1E };
    std::vector<uint8_t> pps = { 0x68, 0xCE, 0x38, 0x80 };
    std::vector<uint8_t> idr(64, 0x65);
    auto stream = makeAnnexB({ sps, pps, idr });
    auto result = AnnexBParser::split(std::span { stream });
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], sps);
    EXPECT_EQ(result[1], pps);
    EXPECT_EQ(result[2], idr);
}

TEST(AnnexBParserTest, SplitOwningEmptyInputReturnsEmpty)
{
    std::vector<uint8_t> empty;
    auto result = AnnexBParser::split(std::span { empty });
    EXPECT_TRUE(result.empty());
}

TEST(AnnexBParserTest, SplitOwningProducesIndependentCopies)
{
    std::vector<uint8_t> nal = { 0x61, 0x02, 0x03 };
    auto stream = makeAnnexB({ nal });
    auto result = AnnexBParser::split(std::span { stream });
    ASSERT_EQ(result.size(), 1u);
    // The copy must not alias the original buffer
    EXPECT_NE(static_cast<const void*>(result[0].data()),
        static_cast<const void*>(stream.data() + 4));
    EXPECT_EQ(result[0], nal);
}

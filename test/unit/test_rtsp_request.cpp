#include <gtest/gtest.h>

#include "rtspserver/rtsp/RTSPRequest.hpp"

using namespace rtspserver::rtsp;

static std::string makeRequest(const std::string& method,
    const std::string& url,
    int cseq,
    const std::string& extra_headers = "",
    const std::string& body = "")
{
    std::string req = method + " " + url + " RTSP/1.0\r\n";
    req += "CSeq: " + std::to_string(cseq) + "\r\n";
    req += extra_headers;
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return req;
}

TEST(RTSPRequestTest, ParseOptions)
{
    auto raw = makeRequest("OPTIONS", "rtsp://localhost:554/test.mp4", 1);
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, "OPTIONS");
    EXPECT_EQ(req->url, "rtsp://localhost:554/test.mp4");
    EXPECT_EQ(req->version, "RTSP/1.0");
    EXPECT_EQ(req->cseq(), 1);
}

TEST(RTSPRequestTest, ParseDescribe)
{
    auto raw = makeRequest("DESCRIBE",
        "rtsp://127.0.0.1:8554/video.h264",
        42,
        "Accept: application/sdp\r\n");
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, "DESCRIBE");
    EXPECT_EQ(req->cseq(), 42);
    EXPECT_EQ(req->header("Accept"), "application/sdp");
}

TEST(RTSPRequestTest, ParseSetupUDP)
{
    std::string transport = "RTP/AVP;unicast;client_port=49152-49153";
    auto raw = makeRequest("SETUP",
        "rtsp://localhost:554/video.mp4/trackID=0",
        3,
        "Transport: " + transport + "\r\n"
                                    "Session: 123456789\r\n");
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, "SETUP");
    EXPECT_EQ(req->header("transport"), transport);
    EXPECT_EQ(req->header("session"), "123456789");
    EXPECT_EQ(req->cseq(), 3);
}

TEST(RTSPRequestTest, ParsePlay)
{
    auto raw = makeRequest("PLAY",
        "rtsp://localhost:554/video.mp4",
        4,
        "Session: 987\r\nRange: npt=0.000-\r\n");
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, "PLAY");
    EXPECT_EQ(req->header("range"), "npt=0.000-");
}

TEST(RTSPRequestTest, HeaderLookupCaseInsensitive)
{
    auto raw = makeRequest("OPTIONS",
        "rtsp://localhost/x",
        1,
        "User-Agent: VLC/3.0\r\n");
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->header("user-agent"), "VLC/3.0");
    EXPECT_EQ(req->header("USER-AGENT"), "VLC/3.0");
}

TEST(RTSPRequestTest, InvalidVersionIsRejected)
{
    std::string raw = "OPTIONS rtsp://localhost:554/ HTTP/1.1\r\n"
                      "CSeq: 1\r\n\r\n";
    auto req = RTSPRequest::parse(raw);
    EXPECT_FALSE(req.has_value());
}

TEST(RTSPRequestTest, MissingCSeqReturnsNegativeOne)
{
    std::string raw = "OPTIONS rtsp://localhost/ RTSP/1.0\r\n\r\n";
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->cseq(), -1);
}

TEST(RTSPRequestTest, BodyParsedFromContentLength)
{
    std::string body = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n";
    auto raw = makeRequest("ANNOUNCE",
        "rtsp://localhost/x",
        5,
        "",
        body);
    auto req = RTSPRequest::parse(raw);

    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->body, body);
}

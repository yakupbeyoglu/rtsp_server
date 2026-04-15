#include "rtspserver/server/RTSPSession.hpp"

#include "rtspserver/media/MediaSource.hpp"
#include "rtspserver/rtp/RTPSender.hpp"

#include "rtspserver/sdp/SDPBuilder.hpp"
#include "rtspserver/utils/Logger.hpp"
#include "rtspserver/utils/StringUtils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace rtspserver::utils;
using namespace rtspserver::utils::StringUtils;
using namespace rtspserver::rtsp;
using namespace rtspserver::rtp;
using namespace rtspserver::media;
using namespace rtspserver::sdp;

namespace rtspserver::server {

namespace fs = std::filesystem;

static std::string makeSessionId()
{
    std::mt19937_64 rng(std::random_device {}());
    return std::to_string(rng());
}

static uint32_t makeSSRC()
{
    std::mt19937 rng(std::random_device {}());
    return static_cast<uint32_t>(rng());
}

static std::string nptRange(double secs)
{
    std::ostringstream oss;
    oss << "npt=" << std::fixed << std::setprecision(3) << secs << "-";
    return oss.str();
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::RTSPSession(int fd, std::string client_ip,
    std::string media_root, int epoll_fd)
    : fd_(fd)
    , epoll_fd_(epoll_fd)
    , client_ip_(std::move(client_ip))
    , media_root_(std::move(media_root))
    , session_id_(makeSessionId())
    , ssrc_(makeSSRC())
    , audio_ssrc_(makeSSRC())
{
    LOG_INFO("Session ", session_id_, " created for client ", client_ip_);
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::~RTSPSession()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    LOG_INFO("Session ", session_id_, " destroyed");
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
bool RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::onReadable(std::vector<RTSPRequest>& out)
{
    char tmp[kRecvChunk];
    while (true) {
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            recv_buf_.append(tmp, static_cast<size_t>(n));
            if (recv_buf_.size() > kMaxRecvBuf) {
                LOG_WARN("Session ", session_id_, ": recv buffer overflow, closing");
                return false;
            }
        } else if (n == 0) {
            return false; // EOF
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            return false;
        }
    }

    // Parse as many complete RTSP messages as the buffer holds.
    while (true) {
        // Discard RFC 2326 §10.12 interleaved binary frames rtcp-client
        while (!recv_buf_.empty() && static_cast<uint8_t>(recv_buf_[0]) == '$') {
            if (recv_buf_.size() < 4)
                goto parse_done;
            const uint16_t plen = (static_cast<uint8_t>(recv_buf_[2]) << 8) | static_cast<uint8_t>(recv_buf_[3]);
            const size_t fsz = 4u + plen;
            if (recv_buf_.size() < fsz)
                goto parse_done;
            recv_buf_.erase(0, fsz);
        }

        if (recv_buf_.empty())
            break;

        {
            const auto end_pos = recv_buf_.find("\r\n\r\n");
            if (end_pos == std::string::npos)
                break;

            const size_t body_start = end_pos + 4;
            std::string header_block = recv_buf_.substr(0, body_start);

            size_t body_len = 0;
            for (const char* hname : { "\r\nContent-Length:", "\r\ncontent-length:" }) {
                size_t cl = header_block.find(hname);
                if (cl != std::string::npos) {
                    size_t vs = header_block.find(':', cl) + 1;
                    size_t ve = header_block.find("\r\n", vs);
                    if (ve != std::string::npos) {
                        try {
                            body_len = std::stoul(
                                StringUtils::trim(header_block.substr(vs, ve - vs)));
                        } catch (...) {
                        }
                    }
                    break;
                }
            }

            if (recv_buf_.size() < body_start + body_len)
                break;

            std::string full = recv_buf_.substr(0, body_start + body_len);
            recv_buf_.erase(0, body_start + body_len);

            auto req = RTSPRequest::parse(full);
            if (req)
                out.push_back(std::move(*req));
        }
    }
parse_done:
    return true;
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
bool RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::onWritable()
{
    while (true) {
        if (!wbuf_) {
            auto opt = outbox_.try_pop();
            if (!opt) {
                // Outbox empty: disarm EPOLLOUT.
                epoll_event ev {};
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = fd_;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
                epollout_armed_.store(false, std::memory_order_release);
                if (close_after_write_.load(std::memory_order_acquire))
                    return false;
                return true;
            }
            wbuf_ = std::move(*opt);
            outbox_bytes_.fetch_sub(
                std::min(wbuf_->size(),
                    outbox_bytes_.load(std::memory_order_relaxed)),
                std::memory_order_relaxed);
            woff_ = 0;
        }

        const auto& buf = *wbuf_;
        const size_t rem = buf.size() - woff_;
        ssize_t n = ::send(fd_, buf.data() + woff_, rem,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            woff_ += static_cast<size_t>(n);
            if (woff_ >= buf.size()) {
                wbuf_.reset();
                woff_ = 0;
            }
        } else if (n == 0) {
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            if (errno == EINTR)
                continue;
            return false;
        }
    }
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::enableWrite()
{
    bool expected = false;
    if (!epollout_armed_.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;
    epoll_event ev {};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    ev.data.fd = fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::pushCommand(RTSPRequest req)
{
    cmd_queue_.push(std::move(req));
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::sendResponse(const RTSPResponse& resp)
{
    std::string wire = resp.toString();
    LOG_DEBUG("Session ", session_id_, " \xe2\x86\x92 ", resp.status, ' ', resp.reason);
    auto buf = std::vector<uint8_t>(wire.begin(), wire.end());
    outbox_bytes_.fetch_add(buf.size(), std::memory_order_relaxed);
    outbox_.push(std::move(buf));
    enableWrite();
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::sendVideoRTP(std::span<const uint8_t> payload,
    uint32_t ts, bool marker)
{
    auto pkt = RTPPacket::build(payload,
        static_cast<uint8_t>(payload_type_),
        rtp_seq_++, ts, ssrc_, marker);
    if (use_tcp_) {
        pushTCPFrame(rtp_channel_, std::span { pkt });
    } else if (rtp_sender_) {
        rtp_sender_->send(std::span { pkt });
    }
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::sendAudioRTP(std::span<const uint8_t> rtp_pkt)
{
    if (use_tcp_) {
        pushTCPFrame(audio_rtp_channel_, rtp_pkt);
    } else if (audio_rtp_sender_) {
        audio_rtp_sender_->send(rtp_pkt);
    }
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::pushTCPFrame(int channel, std::span<const uint8_t> rtp)
{
    if (outbox_bytes_.load(std::memory_order_relaxed) > kOutboxHWM) {
        LOG_WARN("Session ", session_id_,
            ": outbox exceeded high watermark, closing slow consumer");
        close_after_write_.store(true, std::memory_order_release);
        return;
    }
    std::vector<uint8_t> frame;
    frame.reserve(4 + rtp.size());
    frame.push_back('$');
    frame.push_back(static_cast<uint8_t>(channel));
    const uint16_t len = static_cast<uint16_t>(rtp.size());
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    frame.insert(frame.end(), rtp.begin(), rtp.end());
    outbox_bytes_.fetch_add(frame.size(), std::memory_order_relaxed);
    outbox_.push(std::move(frame));
    enableWrite();
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handleRequest(const RTSPRequest& req)
{
    if (req.method == "OPTIONS")
        handleOptions(req);
    else if (req.method == "DESCRIBE")
        handleDescribe(req);
    else if (req.method == "SETUP")
        handleSetup(req);
    else if (req.method == "PLAY")
        handlePlay(req);
    else if (req.method == "PAUSE")
        handlePause(req);
    else if (req.method == "TEARDOWN")
        handleTeardown(req);
    else {
        sendResponse(RTSPResponse::error(req.cseq(), 501, "Not Implemented"));
    }
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handleOptions(const RTSPRequest& req)
{
    sendResponse(RTSPResponse::options(
        req.cseq(),
        "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN"));
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
bool RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::openMedia(const std::string& url, int cseq)
{
    std::string url_path = StringUtils::urlPath(url);
    if (url_path.empty() || url_path == "/") {
        sendResponse(RTSPResponse::error(cseq, 404, "Not Found"));
        return false;
    }

    // Percent-decode before filesystem resolution so that %2e%2e etc.
    // are resolved to their literal characters for the traversal check.
    std::string rel = StringUtils::urlDecode(url_path);
    if (rel.front() == '/')
        rel = rel.substr(1);
    rel = StringUtils::stripTrackSuffix(rel);
    if (!rel.empty() && rel.front() == '/')
        rel = rel.substr(1);

    fs::path full_path = fs::path(media_root_) / rel;

    // Security: prevent path traversal attacks.
    auto canonical_root = fs::weakly_canonical(fs::path(media_root_));
    auto canonical_full = fs::weakly_canonical(full_path);

    std::error_code ec;
    auto rel_check = fs::relative(canonical_full, canonical_root, ec);
    bool escaped = ec || (!rel_check.empty() && *rel_check.begin() == fs::path(".."));
    if (escaped) {
        LOG_WARN("Session ", session_id_, ": path traversal attempt: ", url_path);
        sendResponse(RTSPResponse::error(cseq, 403, "Forbidden"));
        return false;
    }

    if (!fs::exists(full_path)) {
        LOG_WARN("Session ", session_id_, ": file not found: ", full_path.string());
        sendResponse(RTSPResponse::error(cseq, 404, "Not Found"));
        return false;
    }

    media_source_ = std::make_unique<MediaSource>();
    if (!media_source_->open(full_path.string())) {
        sendResponse(RTSPResponse::error(cseq, 415, "Unsupported Media Type"));
        media_source_.reset();
        return false;
    }

    return true;
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handleDescribe(const RTSPRequest& req)
{
    media_source_.reset();

    if (!openMedia(req.url, req.cseq()))
        return;

    std::string url_path = StringUtils::urlPath(req.url);
    std::string rel = url_path.front() == '/' ? url_path.substr(1) : url_path;
    rel = StringUtils::stripTrackSuffix(rel);

    std::string sdp = SDPBuilder::build(
        media_source_->streamInfo(),
        "0.0.0.0",
        rel,
        req.url,
        payload_type_,
        media_source_->audioInfo(),
        audio_payload_type_);

    describe_done_ = true;
    sendResponse(RTSPResponse::describe(req.cseq(), sdp, req.url + '/'));
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handleSetup(const RTSPRequest& req)
{
    if (!describe_done_ || !media_source_) {
        LOG_WARN("Session ", session_id_,
            ": SETUP rejected \xe2\x80\x93 DESCRIBE must precede SETUP");
        sendResponse(RTSPResponse::error(req.cseq(), 455, "Method Not Valid In This State"));
        return;
    }

    std::string transport_hdr = req.header("transport");
    if (transport_hdr.empty()) {
        sendResponse(RTSPResponse::error(req.cseq(), 400, "Bad Request"));
        return;
    }

    bool is_audio_track = (req.url.find("trackID=1") != std::string::npos);

    std::string response_transport;

    if (transport_hdr.find("RTP/AVP/TCP") != std::string::npos || transport_hdr.find("interleaved") != std::string::npos) {
        LOG_INFO("Session ", session_id_, " (", client_ip_, ") transport: RTP/AVP/TCP (interleaved over RTSP/TCP)");
        // tcp selected
        use_tcp_ = true;

        if (is_audio_track) {
            auto il_pos = transport_hdr.find("interleaved=");
            if (il_pos != std::string::npos) {
                il_pos += 12;
                auto dash = transport_hdr.find('-', il_pos);
                if (dash != std::string::npos) {
                    audio_rtp_channel_ = std::stoi(transport_hdr.substr(il_pos, dash - il_pos));
                    audio_rtcp_channel_ = std::stoi(transport_hdr.substr(dash + 1));
                }
            }
            audio_transport_ready_ = true;
            response_transport = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(audio_rtp_channel_) + "-" + std::to_string(audio_rtcp_channel_);
        } else {
            auto il_pos = transport_hdr.find("interleaved=");
            if (il_pos != std::string::npos) {
                il_pos += 12;
                auto dash = transport_hdr.find('-', il_pos);
                if (dash != std::string::npos) {
                    rtp_channel_ = std::stoi(transport_hdr.substr(il_pos, dash - il_pos));
                    rtcp_channel_ = std::stoi(transport_hdr.substr(dash + 1));
                }
            }
            transport_ready_ = true;
            response_transport = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(rtp_channel_) + "-" + std::to_string(rtcp_channel_);
        }
    } else {
        LOG_INFO("Session ", session_id_, " (", client_ip_, ") transport: RTP/AVP/UDP (UDP unicast)");
        // udp selected
        use_tcp_ = false;

        uint16_t client_rtp_port = 0;
        auto cp_pos = transport_hdr.find("client_port=");
        if (cp_pos == std::string::npos) {
            sendResponse(RTSPResponse::error(req.cseq(), 461, "Unsupported Transport"));
            return;
        }
        cp_pos += 12;
        auto dash = transport_hdr.find('-', cp_pos);
        if (dash == std::string::npos)
            dash = transport_hdr.find(';', cp_pos);
        std::string port_str = transport_hdr.substr(cp_pos, dash - cp_pos);
        client_rtp_port = static_cast<uint16_t>(std::stoi(port_str));

        if (is_audio_track) {
            audio_rtp_sender_ = std::make_unique<RTPSender>();
            if (!audio_rtp_sender_->setupUDP(client_ip_, client_rtp_port)) {
                sendResponse(RTSPResponse::error(req.cseq(), 500, "Internal Server Error"));
                return;
            }
            uint16_t srv_port = audio_rtp_sender_->getLocalPort();
            audio_transport_ready_ = true;
            response_transport = "RTP/AVP;unicast;client_port=" + std::to_string(client_rtp_port) + "-" + std::to_string(client_rtp_port + 1u) + ";server_port=" + std::to_string(srv_port) + "-" + std::to_string(static_cast<uint16_t>(srv_port + 1u));
        } else {
            rtp_sender_ = std::make_unique<RTPSender>();
            if (!rtp_sender_->setupUDP(client_ip_, client_rtp_port)) {
                sendResponse(RTSPResponse::error(req.cseq(), 500, "Internal Server Error"));
                return;
            }
            uint16_t srv_port = rtp_sender_->getLocalPort();
            transport_ready_ = true;
            response_transport = "RTP/AVP;unicast;client_port=" + std::to_string(client_rtp_port) + "-" + std::to_string(client_rtp_port + 1u) + ";server_port=" + std::to_string(srv_port) + "-" + std::to_string(static_cast<uint16_t>(srv_port + 1u));
        }
    }

    if (!is_audio_track)
        state_ = State::READY;
    auto setup_resp = RTSPResponse::setup(req.cseq(), session_id_, response_transport);

    sendResponse(setup_resp);
}

// Seek to npt_secs (lands on nearest IDR at or before that position).
// If adjust_offset == true, shift the RTP offset so timestamps continue
// monotonically from last_video_rtp_ (used for scale-play so the decoder
// never sees a backward timestamp).
// Also re-arms first_frame_ so wall_start_ is re-anchored on the next IDR.
template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::seekAndReset(double npt_secs, bool adjust_offset)
{
    if (!media_source_)
        return;

    media_source_->seekToSeconds(npt_secs);

    if (adjust_offset && last_video_rtp_ != 0) {
        const uint32_t idr_rtp = media_source_->rtpOffset();
        if (last_video_rtp_ > idr_rtp)
            media_source_->adjustRTPOffset(last_video_rtp_ - idr_rtp);
    }

    first_frame_ = true;
    pending_vp_.reset();
    pending_ap_.reset();
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handlePlay(const RTSPRequest& req)
{
    if (state_ == State::INIT) {
        sendResponse(RTSPResponse::error(req.cseq(), 455, "Method Not Valid In This State"));
        return;
    }

    const bool is_resume = (state_ == State::PAUSED);

    //  Parse Scale (RFC 2326 §12.34)
    // Variable-rate playback not supported; echo Scale: 1.000.
    // A Speed-change PLAY from VLC always carries Range: npt=<current>- which
    // must NOT be treated as a seek.
    const bool has_scale = !req.header("scale").empty();

    //  Parse Range (RFC 2326 §12.29)
    double npt_start = 0.0;
    double npt_end_val = -1.0;
    bool has_range = false;
    {
        const std::string rh = req.header("range");
        if (!rh.empty()) {
            const auto p = rh.find("npt=");
            if (p != std::string::npos) {
                const std::string val = rh.substr(p + 4);
                const auto dash = val.find('-');
                if (dash != std::string::npos) {
                    const auto start_s = val.substr(0, dash);
                    const auto end_s = val.substr(dash + 1);
                    if (!start_s.empty() && start_s != "now") {
                        try {
                            npt_start = std::stod(start_s);
                            has_range = true;
                        } catch (...) {
                        }
                    }
                    if (!end_s.empty()) {
                        try {
                            npt_end_val = std::stod(end_s);
                        } catch (...) {
                        }
                    }
                }
            }
        }
    }

    // Scale-only: Range is informational (current NPT pos), not a seek.
    if (has_scale)
        has_range = false;

    std::string range_echo;
    std::string rtp_info;

    if (is_resume) {
        // Case A: Resume from PAUSE
        if (pause_start_ != Clock::time_point {}) {
            if (!first_frame_)
                wall_start_ += Clock::now() - pause_start_;
            pause_start_ = {};
        }
        // Do NOT set resume_waiting_for_idr_ on resume!
        range_echo = nptRange(std::max(0.0, last_npt_));

    } else if (has_scale) {
        //  Case B: Scale / speed change
        seekAndReset(last_keyframe_npt_, /*adjust_offset=*/true);
        // Do NOT set resume_waiting_for_idr_ on scale change!
        range_echo = nptRange(std::max(0.0, last_npt_));

    } else {
        // Case C: Fresh play or explicit seek
        double start = has_range ? npt_start : 0.0;
        seekAndReset(start, /*adjust_offset=*/false);

        // Reset all pacing / loop state.
        loop_video_offset_ = 0;
        audio_loop_offset_ = 0;
        last_video_rtp_ = 0;
        last_frame_dur_.reset();
        resume_waiting_for_idr_ = true;
        last_keyframe_npt_ = 0.0;
        last_npt_ = 0.0;
        npt_end_ = npt_end_val;
        pause_start_ = {};

        // rtp info creation
        std::string track_url = req.url;
        if (!track_url.empty() && track_url.back() != '/')
            track_url += '/';
        track_url += "trackID=0";

        uint32_t rtptime = media_source_
            ? media_source_->peekNextVideoRTPTimestamp()
            : 0u;

        std::ostringstream ri;
        ri << "url=" << track_url << ";seq=" << rtp_seq_
           << ";rtptime=" << rtptime;
        if (audio_transport_ready_ && media_source_ && media_source_->audioInfo().present) {
            std::string audio_url = req.url;
            if (!audio_url.empty() && audio_url.back() != '/')
                audio_url += '/';
            audio_url += "trackID=1";
            ri << ",url=" << audio_url
               << ";seq=" << audio_rtp_seq_
               << ";rtptime=" << media_source_->audioRTPOffset();
        }
        rtp_info = ri.str();
        range_echo = has_range ? nptRange(npt_start) : "npt=0.000-";
    }

    state_ = State::PLAYING;
    sendResponse(RTSPResponse::play(req.cseq(), session_id_, range_echo, rtp_info));
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handlePause(const RTSPRequest& req)
{
    if (state_ != State::PLAYING) {
        sendResponse(RTSPResponse::error(req.cseq(), 455, "Method Not Valid In This State"));
        return;
    }
    pause_start_ = Clock::now();
    state_ = State::PAUSED;
    auto resp = RTSPResponse::ok(req.cseq(), session_id_);
    if (media_source_) {
        double cur_secs = last_npt_;
        if (cur_secs < 0.0)
            cur_secs = 0.0;
        std::ostringstream rs;
        rs << "npt=" << std::fixed << std::setprecision(3) << cur_secs << "-";
        resp.headers["Range"] = rs.str();
        LOG_DEBUG("Session ", session_id_, ": PAUSE at npt=", cur_secs, "s");
    }
    sendResponse(resp);
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::handleTeardown(const RTSPRequest& req)
{
    state_ = State::INIT;
    sendResponse(RTSPResponse::ok(req.cseq(), session_id_));
    close_after_write_.store(true, std::memory_order_release);
}

template <std::size_t OutboxHWM, std::size_t MaxRecvBuf, std::size_t RecvChunk>
void RTSPSession<OutboxHWM, MaxRecvBuf, RecvChunk>::pacerTick(std::chrono::steady_clock::time_point now)
{
    // 1. Drain all queued RTSP commands
    while (true) {
        auto req = cmd_queue_.try_pop();
        if (!req)
            break;
        LOG_INFO("Session ", session_id_, " \xe2\x86\x90 ", req->method, ' ', req->url);
        handleRequest(*req);
        if (req->method == "TEARDOWN")
            return;
    }

    //  2. If not playing or transport not ready, just track pause time
    if (state_ != State::PLAYING || !media_source_ || !transport_ready_) {
        if (state_ == State::PAUSED && pause_start_ == Clock::time_point {})
            pause_start_ = now;
        if (state_ != State::PAUSED)
            pending_vp_.reset();
        return;
    }

    // 3. Adjust wall_start_ after pause→resume
    // Skip the delta when first_frame_=true: step 6 will anchor
    // wall_start_ = now when the first IDR is actually sent.
    if (pause_start_ != Clock::time_point {}) {
        if (!first_frame_)
            wall_start_ += now - pause_start_;
        pause_start_ = {};
    }

    //  4. On seek / first-play, discard any held frame (stale)
    if (first_frame_)
        pending_vp_.reset();

    //  4b. On seek/Range/PLAY, gate on next IDR
    if (resume_waiting_for_idr_) {
        if (pending_vp_ && !pending_vp_->is_key_frame) {
            pending_vp_.reset();
            return; // Wait for IDR
        }
        // If we have an IDR, clear the gate and proceed
        if (pending_vp_ && pending_vp_->is_key_frame)
            resume_waiting_for_idr_ = false;
    }

    //  5. Read next video frame (if none pending)
    if (!pending_vp_) {
        VideoPacket vp;
        if (!media_source_->readPacket(vp)) {
            LOG_DEBUG("Session ", session_id_, ": EOF, looping");
            media_source_->seek(last_video_rtp_
                + last_frame_dur_.value_or(0));
            first_frame_ = true;
            loop_video_offset_ = 0;
            audio_loop_offset_ = 0;
            last_video_rtp_ = 0;
            resume_waiting_for_idr_ = false;
            npt_end_ = -1.0;
            pending_ap_.reset();
            return;
        }

        if (vp.nals.empty())
            return;

        // IDR gate: discard non-keyframes until we see the first IDR.
        if ((first_frame_ || resume_waiting_for_idr_) && !vp.is_key_frame) {
            return;
        }
        resume_waiting_for_idr_ = false;

        if (npt_end_ > 0.0) {
            double current_npt = static_cast<double>(vp.rtp_timestamp) / 90000.0;
            if (current_npt >= npt_end_) {
                LOG_DEBUG("Session ", session_id_, ": reached npt_end=", npt_end_);
                state_ = State::PAUSED;
                return;
            }
        }

        pending_vp_ = std::move(vp);
    }

    if (resume_waiting_for_idr_ && pending_vp_ && pending_vp_->is_key_frame)
        resume_waiting_for_idr_ = false;

    //  6. Establish wall-clock anchor
    if (first_frame_) {
        wall_start_ = now;
        loop_video_offset_ = pending_vp_->rtp_timestamp;
        const int sr = media_source_->audioInfo().sample_rate;
        if (sr > 0) {
            audio_loop_offset_ = static_cast<uint32_t>(
                static_cast<double>(loop_video_offset_) / 90000.0 * sr);
        }
        first_frame_ = false;
    }

    //  7. Timing: if frame not yet due, come back next tick
    using Duration = std::chrono::duration<double>;
    double media_secs = static_cast<double>(
                            pending_vp_->rtp_timestamp - loop_video_offset_)
        / 90000.0;
    auto send_time = wall_start_
        + std::chrono::duration_cast<Clock::duration>(Duration(media_secs));

    if (now < send_time)
        return;

    {
        auto late_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - send_time)
                           .count();
        LOG_INFO("Session ", session_id_,
            ": [video] ", pending_vp_->is_key_frame ? "IDR" : "P  ",
            " seq=", rtp_seq_,
            " rtp=", pending_vp_->rtp_timestamp,
            " npt=", std::fixed, std::setprecision(3), media_secs, "s",
            " late=+", late_ms, "ms");
    }

    //  8. Build and send video RTP packets
    {
        const auto& info = media_source_->streamInfo();
        types::NalUnitViews nal_list;

        if (pending_vp_->is_key_frame) {
            if (!info.sps.empty())
                nal_list.emplace_back(info.sps);
            if (!info.pps.empty())
                nal_list.emplace_back(info.pps);
        }

        for (const auto& nal : pending_vp_->nals) {
            if (nal.empty())
                continue;
            uint8_t nal_type = nal[0] & 0x1Fu;
            if (pending_vp_->is_key_frame && (nal_type == 7u || nal_type == 8u))
                continue;
            nal_list.push_back(nal);
        }

        const size_t last_nal_idx = nal_list.empty() ? 0 : nal_list.size() - 1;
        for (size_t ni = 0; ni < nal_list.size(); ++ni) {
            const auto nal = nal_list[ni];
            bool last_nal = (ni == last_nal_idx);

            auto payloads = packetizer_.packetize(nal);
            LOG_DEBUG("Session ", session_id_,
                ": [nal]  type=", static_cast<int>(nal[0] & 0x1Fu),
                " size=", nal.size(), "B",
                " frags=", payloads.size());
            for (size_t pi = 0; pi < payloads.size(); ++pi) {
                auto& p = payloads[pi];
                bool mk = last_nal && (pi == payloads.size() - 1);
                sendVideoRTP(std::span { p.payload },
                    pending_vp_->rtp_timestamp, mk);
            }
        }
    }

    //  8b. Update tracking state
    const double abs_npt_secs = static_cast<double>(media_source_->rtpOffset()) / 90000.0 + media_secs;

    if (pending_vp_->is_key_frame)
        last_keyframe_npt_ = abs_npt_secs;

    last_npt_ = abs_npt_secs;

    if (last_video_rtp_ != 0 && pending_vp_->rtp_timestamp > last_video_rtp_)
        last_frame_dur_ = pending_vp_->rtp_timestamp - last_video_rtp_;

    last_video_rtp_ = pending_vp_->rtp_timestamp;
    pending_vp_.reset();

    //  9. Paced audio
    if (audio_transport_ready_) {
        const int sample_rate = media_source_->audioInfo().sample_rate;
        if (sample_rate > 0) {
            auto sendOneAudio = [&](const AudioPacket& ap) {
                if (ap.data.empty())
                    return;
                uint16_t au_size = static_cast<uint16_t>(ap.data.size());
                std::vector<uint8_t> pkt_buf;
                pkt_buf.reserve(4 + ap.data.size());
                pkt_buf.push_back(0x00);
                pkt_buf.push_back(0x10);
                pkt_buf.push_back(static_cast<uint8_t>((au_size >> 5) & 0xFF));
                pkt_buf.push_back(static_cast<uint8_t>((au_size & 0x1F) << 3));
                pkt_buf.insert(pkt_buf.end(), ap.data.begin(), ap.data.end());
                auto pkt = RTPPacket::build(
                    std::span { pkt_buf },
                    static_cast<uint8_t>(audio_payload_type_),
                    audio_rtp_seq_++,
                    ap.rtp_timestamp,
                    audio_ssrc_,
                    true);
                LOG_DEBUG("Session ", session_id_,
                    ": [audio] seq=", audio_rtp_seq_ - 1u,
                    " rtp=", ap.rtp_timestamp,
                    " sz=", ap.data.size());
                sendAudioRTP(std::span { pkt });
            };

            auto audioIsDue = [&](const AudioPacket& ap) -> bool {
                double asecs = static_cast<double>(static_cast<uint32_t>(
                                   ap.rtp_timestamp - audio_loop_offset_))
                    / static_cast<double>(sample_rate);
                auto audio_send_time = wall_start_
                    + std::chrono::duration_cast<Clock::duration>(Duration(asecs));
                return now >= audio_send_time;
            };

            if (pending_ap_ && audioIsDue(*pending_ap_)) {
                sendOneAudio(*pending_ap_);
                pending_ap_.reset();
            }
            if (!pending_ap_) {
                AudioPacket ap;
                while (media_source_->readAudioPacket(ap)) {
                    if (!audioIsDue(ap)) {
                        pending_ap_ = std::move(ap);
                        break;
                    }
                    sendOneAudio(ap);
                }
            }
        }
    }
}

// Explicit instantiation of the default configuration
template class RTSPSession<>;

} // namespace rtspserver::server

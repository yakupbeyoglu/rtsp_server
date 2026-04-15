#include "rtspserver/rtp/RTPSender.hpp"

#include "rtspserver/utils/Logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace rtspserver::rtp {

RTPSender::~RTPSender() { close(); }

void RTPSender::close()
{
    if (mode_ == Mode::UDP && udp_fd_ >= 0) {
        ::close(udp_fd_);
        udp_fd_ = -1;
    }
    // rtsp session class responsible to close the tcp fd when session teardown happens.
    tcp_fd_ = -1;
    ready_ = false;
    mode_ = Mode::NONE;
}

bool RTPSender::setupUDP(const std::string& remote_ip,
    uint16_t remote_rtp_port,
    uint16_t local_port)
{
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        LOG_ERROR("RTPSender: socket(): ", std::strerror(errno));
        return false;
    }

    // Bind to a local port if requested, 0 is os pick
    if (local_port != 0) {
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(local_port);
        if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            LOG_WARN("RTPSender: bind() local port ", local_port, ": ", std::strerror(errno));
        }
    }

    // Record the remote address for sendto()
    sockaddr_in remote {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(remote_rtp_port);
    if (::inet_pton(AF_INET, remote_ip.c_str(), &remote.sin_addr) != 1) {
        LOG_ERROR("RTPSender: invalid remote IP: ", remote_ip);
        ::close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    // "Connect" the UDP socket so we can use send() instead of sendto()
    if (::connect(udp_fd_, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
        LOG_ERROR("RTPSender: connect(): ", std::strerror(errno));
        ::close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    mode_ = Mode::UDP;
    ready_ = true;
    LOG_DEBUG("RTPSender: UDP ready → ", remote_ip, ':', remote_rtp_port);
    return true;
}

uint16_t RTPSender::getLocalPort() const
{
    if (mode_ != Mode::UDP || udp_fd_ < 0)
        return 0;
    sockaddr_in addr {};
    socklen_t len = sizeof(addr);
    if (::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        return ntohs(addr.sin_port);
    return 0;
}

void RTPSender::setupTCP(int tcp_fd, int rtp_channel, int /*rtcp_channel*/)
{
    tcp_fd_ = tcp_fd;
    rtp_channel_ = static_cast<uint8_t>(rtp_channel);
    mode_ = Mode::TCP;
    ready_ = true;
    LOG_DEBUG("RTPSender: TCP interleaved ready, channel=", rtp_channel);
}

bool RTPSender::send(std::span<const uint8_t> rtp_packet)
{
    if (!ready_)
        return false;
    if (mode_ == Mode::UDP)
        return sendUDP(rtp_packet);
    return sendTCPInterleaved(rtp_packet);
}

bool RTPSender::sendUDP(std::span<const uint8_t> data)
{
    ssize_t sent = ::send(udp_fd_,
        data.data(),
        data.size(),
        MSG_NOSIGNAL);
    if (sent < 0) {
        LOG_WARN("RTPSender: send(): ", std::strerror(errno));
        return false;
    }
    return true;
}

bool RTPSender::sendTCPInterleaved(std::span<const uint8_t> data)
{
    // RTSP interleaved framing: $ | channel | length-hi | length-lo | data
    if (data.size() > 0xFFFFu)
        return false;

    uint8_t hdr[4] = {
        '$',
        rtp_channel_,
        static_cast<uint8_t>(data.size() >> 8),
        static_cast<uint8_t>(data.size()),
    };

    // Use scatter-gather I/O (sendmsg) so the framing header and the RTP
    // payload are sent in a single syscall without copying the payload into
    // an intermediate buffer.
    iovec iov[2] = {
        { hdr, 4 },
        { const_cast<uint8_t*>(data.data()), data.size() },
    };
    msghdr msg {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    std::lock_guard<std::mutex> lk(tcp_write_mutex_);
    size_t remaining = 4 + data.size();
    while (remaining > 0) {
        ssize_t n = ::sendmsg(tcp_fd_, &msg, MSG_NOSIGNAL);
        if (n <= 0) {
            LOG_WARN("RTPSender: TCP sendmsg(): ", std::strerror(errno));
            return false;
        }
        // Advance the scatter-gather vectors over partial writes (rare in
        // practice for small RTP frames, but required for correctness).
        auto advance = static_cast<size_t>(n);
        remaining -= advance;
        while (advance > 0 && msg.msg_iovlen > 0) {
            if (advance >= msg.msg_iov[0].iov_len) {
                advance -= msg.msg_iov[0].iov_len;
                ++msg.msg_iov;
                --msg.msg_iovlen;
            } else {
                msg.msg_iov[0].iov_base = static_cast<char*>(msg.msg_iov[0].iov_base) + advance;
                msg.msg_iov[0].iov_len -= advance;
                advance = 0;
            }
        }
    }
    return true;
}

} // namespace rtspserver::rtp

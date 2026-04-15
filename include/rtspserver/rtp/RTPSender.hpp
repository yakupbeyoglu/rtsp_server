#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "rtspserver/rtp/IRTPSender.hpp"

namespace rtspserver::rtp {

/**
 * @brief Rtp Sender class that implements the IRTPSender interface.
 * It can send RTP packets over UDP or TCP (interleaved) transport.
 * UDP, each packet is sent as a UDP datagram to client_ip:rtp_port.
 * TCP, packets are sent over the established RTSP TCP connection using
 * RTSP interleaved framing (RFC 2326 §10.12).
 */
class RTPSender : public IRTPSender {
public:
    RTPSender() = default;
    ~RTPSender() override;

    [[nodiscard]] bool setupUDP(const std::string& remote_ip,
        uint16_t remote_rtp_port,
        uint16_t local_port = 0) override;

    void setupTCP(int tcp_fd, int rtp_channel, int rtcp_channel) override;

    [[nodiscard]] bool send(std::span<const uint8_t> rtp_packet) override;

    bool isReady() const override { return ready_; }
    uint16_t getLocalPort() const override;
    void close() override;

    std::mutex& writeMutex() { return tcp_write_mutex_; }

private:
    bool sendUDP(std::span<const uint8_t> data);
    bool sendTCPInterleaved(std::span<const uint8_t> data);

    enum class Mode { NONE,
        UDP,
        TCP };
    Mode mode_ { Mode::NONE };
    bool ready_ { false };

    // UDP state
    int udp_fd_ { -1 };

    // TCP state
    int tcp_fd_ { -1 };
    uint8_t rtp_channel_ { 0 };
    std::mutex tcp_write_mutex_;
};

} // namespace rtspserver::rtp

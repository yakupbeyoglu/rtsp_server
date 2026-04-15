#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace rtspserver::rtp {

/**
 * @brief Abstract RTP sender interface.
 *
 * Callers configure the transport once via setupUDP() or setupTCP(), then
 * call send() for each RTP packet.  The interface is intentionally minimal —
 * transport-specific internals (sockets, mutexes, etc.) belong in the
 * concrete implementation.
 */
class IRTPSender {
public:
    virtual ~IRTPSender() = default;

    // Non-copyable
    IRTPSender(const IRTPSender&) = delete;
    IRTPSender& operator=(const IRTPSender&) = delete;

    /**
     * @brief Bind a UDP socket and point it at @p remote_ip : @p remote_rtp_port.
     * @param remote_ip
     * @param remote_rtp_port
     * @param local_port 0 lets the OS choose an ephemeral port.
     * @return true on success, false on failure.
     */
    [[nodiscard]] virtual bool setupUDP(const std::string& remote_ip,
        uint16_t remote_rtp_port,
        uint16_t local_port = 0)
        = 0;

    /**
     * @brief Configure for TCP-interleaved (RFC 2326 §10.12) delivery.
     * @param tcp_fd The file descriptor of the TCP connection.
     * @param rtp_channel The RTP channel number.
     * @param rtcp_channel The RTCP channel number.
     */
    virtual void setupTCP(int tcp_fd, int rtp_channel, int rtcp_channel) = 0;

    /// Transmit a fully-formed RTP packet.  Returns false on send failure.

    /**
     * @brief Help to transfer fully-formet rtp packet.
     * @return false in case send failure
     * @return true inb case send success
     */
    [[nodiscard]] virtual bool send(std::span<const uint8_t> rtp_packet) = 0;

    /**
     * @brief Check udp,tcp  setup is correctly done
     */
    virtual bool isReady() const = 0;

    virtual uint16_t getLocalPort() const = 0;

    virtual void close() = 0;

protected:
    IRTPSender() = default;
};

} // namespace rtspserver::rtp

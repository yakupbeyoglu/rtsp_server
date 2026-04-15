#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rtspserver::server {

template <std::size_t>
class Reactor; // forward declaration

/**
 * @brief RTSP Server is a thin facade around the Reactor class,
 * responsible for initialization and configuration.
 *
 */
class RTSPServer {
public:
    RTSPServer(std::string host,
        uint16_t port,
        std::string media_root);
    ~RTSPServer();

    RTSPServer(const RTSPServer&) = delete;
    RTSPServer& operator=(const RTSPServer&) = delete;

    // Bind the socket and enter the event loop. Blocks until stop() or signal.
    [[nodiscard]] bool run();

    void stop();

private:
    std::string host_;
    uint16_t port_;
    std::string media_root_;

    std::unique_ptr<Reactor<4>> reactor_;
};

} // namespace rtspserver::server

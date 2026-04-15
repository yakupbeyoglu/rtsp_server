#include "rtspserver/server/RTSPServer.hpp"

#include "rtspserver/server/Reactor.hpp"

namespace rtspserver::server {

RTSPServer::RTSPServer(std::string host,
    uint16_t port,
    std::string media_root)
    : host_(std::move(host))
    , port_(port)
    , media_root_(std::move(media_root))
{
}

RTSPServer::~RTSPServer() = default;

bool RTSPServer::run()
{
    reactor_ = std::make_unique<Reactor<4>>(host_, port_, media_root_);
    return reactor_->run();
}

void RTSPServer::stop()
{
    if (reactor_)
        reactor_->stop();
}

} // namespace rtspserver::server

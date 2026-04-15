#include "rtspserver/server/RTSPServer.hpp"
#include "rtspserver/utils/Logger.hpp"
#include "rtspserver/utils/StringUtils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace rtspserver::utils;
using namespace rtspserver::utils::StringUtils;
using namespace rtspserver::server;

namespace fs = std::filesystem;

static void printUsage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [<directory>] [<host:port>]\n"
        << '\n'
        << "  <directory>  Root folder containing video files.\n"
        << "               Default: current working directory.\n"
        << "  <host:port>  Address to bind the server.\n"
        << "               Default: 0.0.0.0:554.\n"
        << '\n'
        << "Example:\n"
        << "  " << argv0 << " /videos 0.0.0.0:8554\n"
        << '\n'
        << "Stream with VLC:\n"
        << "  vlc rtsp://<host>:<port>/<filename>\n";
}

int main(int argc, char* argv[])
{
    std::string media_dir = fs::current_path().string();
    std::string host = "0.0.0.0";
    uint16_t port = 554;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }

        if (arg == "-v" || arg == "--verbose") {
            Logger::instance().setLevel(LogLevel::DEBUG);
            continue;
        }

        // host:port argument (contains ':')
        if (arg.find(':') != std::string::npos) {
            std::string h;
            uint16_t p = 0;
            if (!StringUtils::parseHostPort(arg, h, p)) {
                std::cerr << "Error: invalid host:port '" << arg << "'\n";
                return 1;
            }
            host = h;
            port = p;
            continue;
        }

        // Otherwise treat as directory
        media_dir = arg;
    }

    //  Validate media directory
    if (!fs::is_directory(media_dir)) {
        std::cerr << "Error: '" << media_dir
                  << "' is not a directory.\n";
        printUsage(argv[0]);
        return 1;
    }
    media_dir = fs::canonical(media_dir).string();

    LOG_INFO("Media root : ", media_dir);
    LOG_INFO("Bind address: rtsp://", host, ':', port, '/');

    // ── Start server ─────────────────────────────────────────────────────────
    // Signal handling (SIGINT / SIGTERM) is managed internally by RTSPServer
    // via a signalfd registered on its epoll instance — no global pointer needed.
    RTSPServer server(host, port, media_dir);

    if (!server.run()) {
        LOG_ERROR("Server failed to start");
        return 1;
    }

    LOG_INFO("Server shut down cleanly.");
    return 0;
}

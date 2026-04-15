#pragma once

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace rtspserver::utils::SocketUtil {

/**
 * @brief Helper to set queue as non blocking mode
 *
 * @param fd
 * @return true
 * @return false
 */
inline bool setNonBlocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/**
 * @brief Helper to set TCP_NODELAY option
 *
 * @param fd
 * @return true
 * @return false
 */
inline bool setTcpNoDelay(int fd)
{
    int flag = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

} // namespace rtspserver::utils::SocketUtil

#include "rtspserver/server/Reactor.hpp"
#include "rtspserver/server/RTSPSession.hpp"
#include "rtspserver/utils/Logger.hpp"
#include "rtspserver/utils/SocketUtil.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <chrono>
#include <vector>

using namespace rtspserver::utils;
using namespace rtspserver::rtsp;

namespace rtspserver::server {

static constexpr long kPacerIntervalMs = 30; // ~30 ms pacing clock

template <std::size_t PacerWorkers>
Reactor<PacerWorkers>::Reactor(std::string host, uint16_t port, std::string media_root)
    : host_(std::move(host))
    , port_(port)
    , media_root_(std::move(media_root))
{
}

template <std::size_t PacerWorkers>
Reactor<PacerWorkers>::~Reactor() { stop(); }

template <std::size_t PacerWorkers>
bool Reactor<PacerWorkers>::run()
{
    server_fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!server_fd_) {
        LOG_ERROR("Reactor: socket: ", std::strerror(errno));
        return false;
    }

    int opt = 1;
    ::setsockopt(server_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(server_fd_.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (host_.empty() || host_ == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            LOG_ERROR("Reactor: invalid bind address '", host_, "'");
            return false;
        }
    }
    if (::bind(server_fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Reactor: bind: ", std::strerror(errno));
        return false;
    }
    if (::listen(server_fd_.get(), 64) < 0) {
        LOG_ERROR("Reactor: listen: ", std::strerror(errno));
        return false;
    }

    //  Dispatcher epoll
    epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd_) {
        LOG_ERROR("Reactor: epoll_create1: ", std::strerror(errno));
        return false;
    }

    //  Wakeup eventfd (stop())
    wake_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_) {
        LOG_ERROR("Reactor: wake eventfd: ", std::strerror(errno));
        return false;
    }

    // signalfd (SIGINT / SIGTERM)
    sigset_t sigs;
    ::sigemptyset(&sigs);
    ::sigaddset(&sigs, SIGINT);
    ::sigaddset(&sigs, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    signal_fd_.reset(::signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signal_fd_) {
        LOG_ERROR("Reactor: signalfd: ", std::strerror(errno));
        ::pthread_sigmask(SIG_UNBLOCK, &sigs, nullptr);
        return false;
    }

    //  Dispatcher to  Logic eventfd
    // Logic thread blocks on read(); Dispatcher writes 1 for each parsed request.
    logic_efd_.reset(::eventfd(0, EFD_CLOEXEC));
    if (!logic_efd_) {
        LOG_ERROR("Reactor: logic eventfd: ", std::strerror(errno));
        return false;
    }

    //  Logic to Pacer eventfd
    // Non-blocking so Logic never blocks when Pacer's epoll queue is full.
    pacer_efd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!pacer_efd_) {
        LOG_ERROR("Reactor: pacer eventfd: ", std::strerror(errno));
        return false;
    }

    //  timerfd: pacing clock
    timer_fd_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!timer_fd_) {
        LOG_ERROR("Reactor: timerfd_create: ", std::strerror(errno));
        return false;
    }
    {
        itimerspec its {};
        its.it_value.tv_nsec = kPacerIntervalMs * 1'000'000L;
        its.it_interval.tv_nsec = kPacerIntervalMs * 1'000'000L;
        ::timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    }

    // ── Pacer epoll: timerfd + pacer_efd ────────────────────────────────────
    pacer_epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!pacer_epoll_) {
        LOG_ERROR("Reactor: pacer epoll: ", std::strerror(errno));
        return false;
    }

    auto addEpoll = [](int epfd, int fd, uint32_t events) {
        epoll_event ev {};
        ev.events = events;
        ev.data.fd = fd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };

    // Dispatcher epoll: server socket + control fds
    addEpoll(epoll_fd_.get(), server_fd_.get(), EPOLLIN);
    addEpoll(epoll_fd_.get(), wake_fd_.get(), EPOLLIN);
    addEpoll(epoll_fd_.get(), signal_fd_.get(), EPOLLIN);

    // Pacer epoll: timer + wakeup
    addEpoll(pacer_epoll_.get(), timer_fd_.get(), EPOLLIN);
    addEpoll(pacer_epoll_.get(), pacer_efd_.get(), EPOLLIN);

    running_.store(true);
    LOG_INFO("Reactor: listening on rtsp://", host_, ':', port_, '/');

    // Start Logic and Pacer first so they're ready for events.
    logic_thread_ = std::jthread([this](std::stop_token st) { logicLoop(std::move(st)); });
    pacer_thread_ = std::jthread([this](std::stop_token st) { pacerLoop(std::move(st)); });
    dispatcher_thread_ = std::jthread([this](std::stop_token st) { dispatcherLoop(std::move(st)); });

    // Block until the Dispatcher exits (signal or stop()).
    if (dispatcher_thread_.joinable())
        dispatcher_thread_.join();

    // Orderly shutdown: stop the other two threads.
    logic_thread_.request_stop();
    pacer_thread_.request_stop();

    // Unblock Logic (it's blocked on read(logic_efd_)).
    {
        uint64_t v = 1;
        ::write(logic_efd_.get(), &v, sizeof(v));
    }

    if (logic_thread_.joinable())
        logic_thread_.join();
    if (pacer_thread_.joinable())
        pacer_thread_.join();

    // Drain all in-flight pacerTick() tasks before destroying session objects.
    pacer_pool_.shutdown();

    // Close all remaining sessions.
    {
        std::unique_lock lk(sessions_mtx_);
        sessions_.clear();
    }

    ::pthread_sigmask(SIG_UNBLOCK, &sigs, nullptr);
    return true;
}

template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::stop()
{
    if (!running_.exchange(false))
        return;
    if (wake_fd_.valid()) {
        uint64_t v = 1;
        ::write(wake_fd_.get(), &v, sizeof(v));
    }
}

template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::dispatcherLoop(std::stop_token stoken)
{
    std::stop_callback sc(stoken, [this] { stop(); });

    epoll_event events[64];
    while (running_.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epoll_fd_.get(), events, 64, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG_WARN("Reactor: epoll_wait: ", std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t ev = events[i].events;

            //  Control fds: shutdown
            if (fd == wake_fd_.get() || fd == signal_fd_.get()) {
                if (fd == signal_fd_.get()) {
                    signalfd_siginfo info {};
                    ::read(signal_fd_.get(), &info, sizeof(info));
                    LOG_INFO("Reactor: signal ", info.ssi_signo, " – shutting down");
                }
                running_.store(false);
                goto done;
            }

            //  Server socket: accept new
            if (fd == server_fd_.get()) {
                acceptClient();
                continue;
            }

            //  Client fds
            if (ev & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                closeSession(fd);
                continue;
            }

            {
                std::shared_ptr<RTSPSession<>> session;
                {
                    std::shared_lock lk(sessions_mtx_);
                    auto it = sessions_.find(fd);
                    if (it != sessions_.end())
                        session = it->second;
                }
                if (!session)
                    continue;

                // Egress: drain Outbox first (EPOLLOUT before EPOLLIN avoids
                // buffering delays when command+response fit in one epoll wake).
                if (ev & EPOLLOUT) {
                    if (!session->onWritable()) {
                        closeSession(fd);
                        continue;
                    }
                }

                if (ev & EPOLLIN) {
                    std::vector<RTSPRequest> parsed;
                    if (!session->onReadable(parsed)) {
                        closeSession(fd);
                        continue;
                    }
                    // Route each parsed request through Dispatcher→Logic pipeline.
                    for (auto& req : parsed) {
                        LOG_INFO("Session ", session->sessionId(),
                            " ← ", req.method, ' ', req.url);
                        cmd_queue_.push({ fd, std::move(req) });
                        uint64_t v = 1;
                        ::write(logic_efd_.get(), &v, sizeof(v));
                    }
                }
            }
        }
    }
done:;
}

template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::logicLoop(std::stop_token stoken)
{
    while (!stoken.stop_requested()) {
        // Block until the Dispatcher (or shutdown) signals us.
        uint64_t v;
        ssize_t n = ::read(logic_efd_.get(), &v, sizeof(v));
        if (n < 0 && errno == EINTR)
            continue;

        if (stoken.stop_requested())
            break;

        // Drain the global command queue.
        while (auto opt = cmd_queue_.try_pop()) {
            auto& [client_fd, req] = *opt;

            std::shared_ptr<RTSPSession<>> session;
            {
                std::shared_lock lk(sessions_mtx_);
                auto it = sessions_.find(client_fd);
                if (it != sessions_.end())
                    session = it->second;
            }
            if (!session)
                continue;

            // Push to the per-session queue; Pacer will drain it on
            // its next tick, run handleRequest(), update session state.
            session->pushCommand(std::move(req));

            // Wake the Pacer so RTSP responses are generated promptly
            // (rather than waiting up to 30 ms for the next timer tick).
            uint64_t w = 1;
            ::write(pacer_efd_.get(), &w, sizeof(w));
        }
    }
}

//  Pacer Loop
// Wakes on the 30 ms timerfd OR immediately when Logic writes to pacer_efd.
// For every wake, iterates all sessions: drain commands, then tick PLAYING ones.
template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::pacerLoop(std::stop_token stoken)
{
    using Clock = std::chrono::steady_clock;

    epoll_event events[2];
    while (!stoken.stop_requested()) {
        int n = ::epoll_wait(pacer_epoll_.get(), events, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            // Drain the fd so it re-arms for the next wake.
            uint64_t v;
            ::read(events[i].data.fd, &v, sizeof(v));
        }

        if (stoken.stop_requested())
            break;

        // Snapshot the session list so we don't hold the map lock while
        // doing (potentially slow) media I/O and frame sends.
        std::vector<std::shared_ptr<RTSPSession<>>> active;
        {
            std::shared_lock lk(sessions_mtx_);
            active.reserve(sessions_.size());
            for (auto& [fd, s] : sessions_)
                active.push_back(s);
        }

        auto now = Clock::now();
        for (auto& session : active) {
            // Claim the in-flight slot; if the previous tick is still running
            // on a pool worker, skip this session for this timer cycle.
            if (!session->tryClaimPacerTick())
                continue;

            pacer_pool_.submit([session, now]() {
                session->pacerTick(now);
                session->releasePacerTick();
            });
        }
    }
}

template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::acceptClient()
{
    while (running_.load(std::memory_order_relaxed)) {
        sockaddr_in caddr {};
        socklen_t clen = sizeof(caddr);
        UniqueFd cfd { ::accept4(server_fd_.get(),
            reinterpret_cast<sockaddr*>(&caddr),
            &clen,
            SOCK_NONBLOCK | SOCK_CLOEXEC) };
        if (!cfd) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            LOG_WARN("Reactor: accept4: ", std::strerror(errno));
            break;
        }

        SocketUtil::setTcpNoDelay(cfd.get());

        // Limit the kernel TCP send buffer.  Without this bound the kernel
        // may buffer several seconds of RTP data before a PAUSE command from
        // VLC reaches the Pacer, making playback appear to continue long after
        // the user pressed stop.  128 KB keeps at most ~300 ms of a 3 Mbps
        // stream in flight, which is enough for smooth playback without the
        // multi-second PAUSE lag.
        {
            int sndbuf = 128 * 1024;
            ::setsockopt(cfd.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        }

        char ip_buf[INET_ADDRSTRLEN] = "unknown";
        ::inet_ntop(AF_INET, &caddr.sin_addr, ip_buf, sizeof(ip_buf));
        LOG_INFO("Reactor: new connection from ", ip_buf, ':', ntohs(caddr.sin_port));

        const int raw_fd = cfd.release();

        // Create session (passes epoll_fd so it can arm EPOLLOUT itself).
        auto session = std::make_shared<RTSPSession<>>(
            raw_fd, std::string(ip_buf), media_root_, epoll_fd_.get());

        {
            std::unique_lock lk(sessions_mtx_);
            sessions_.emplace(raw_fd, session);
        }

        // Register the client fd for read events + hangup detection.
        epoll_event ev {};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = raw_fd;
        if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, raw_fd, &ev) < 0) {
            LOG_WARN("Reactor: epoll_ctl ADD fd=", raw_fd, ": ", std::strerror(errno));
            std::unique_lock lk(sessions_mtx_);
            sessions_.erase(raw_fd);
        }
    }
}

template <std::size_t PacerWorkers>
void Reactor<PacerWorkers>::closeSession(int fd)
{
    // Remove from epoll first so the Dispatcher stops seeing events for fd.
    ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);

    std::shared_ptr<RTSPSession<>> session;
    {
        std::unique_lock lk(sessions_mtx_);
        auto it = sessions_.find(fd);
        if (it != sessions_.end()) {
            session = std::move(it->second);
            sessions_.erase(it);
        }
    }

    LOG_DEBUG("Reactor: session fd=", fd, " closed");
}

// ── Explicit instantiation for the default 4-worker configuration ──────────
template class Reactor<4>;

} // namespace rtspserver::server

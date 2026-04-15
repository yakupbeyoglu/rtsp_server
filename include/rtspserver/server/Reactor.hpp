#pragma once

#include "rtspserver/rtsp/RTSPRequest.hpp"
#include "rtspserver/server/RTSPSession.hpp"
#include "rtspserver/utils/NonBlockingQueue.hpp"
#include "rtspserver/utils/ThreadPool.hpp"
#include "rtspserver/utils/UniqueFd.hpp"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace rtspserver::server {

/**
 * @brief Reactor class that manages RTSP sessions, dispatches I/O events, and orchestrates pacing.
 *
 * The Reactor class implements a micro-reactor pattern with three main components:
 * 1. Dispatcher: Handles I/O events.
 * 2. Logic: Routes requests and manages session state.
 * 3. Pacer Orchestrator: Uses a timerfd to submit per-session pacerTick() tasks to a ThreadPool.
 *    One in-flight slot per session is enforced via RTSPSession::tryClaimPacerTick() / releasePacerTick().
 *
 * @tparam PacerWorkers The number of worker threads in the pacer thread pool.
 */
template <std::size_t PacerWorkers = 4>
class Reactor {
public:
    Reactor(std::string host, uint16_t port, std::string media_root);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    /**
     * @brief Bind, start threads, and block until a signal or stop() is called.
     *
     * @return true
     * @return false
     */
    [[nodiscard]] bool run();

    /**
     * @brief Stop the reactor by signaling the dispatcher.
     *
     */
    void stop();

private:
    void dispatcherLoop(std::stop_token);
    void pacerLoop(std::stop_token);
    void logicLoop(std::stop_token);

    void acceptClient();
    void closeSession(int fd);

    std::string host_;
    uint16_t port_;
    std::string media_root_;

    utils::UniqueFd server_fd_;
    utils::UniqueFd epoll_fd_;
    utils::UniqueFd wake_fd_;
    utils::UniqueFd signal_fd_;
    utils::UniqueFd logic_efd_;
    utils::UniqueFd pacer_efd_;
    utils::UniqueFd timer_fd_;
    utils::UniqueFd pacer_epoll_;

    std::atomic<bool> running_ { false };

    mutable std::shared_mutex sessions_mtx_;
    std::unordered_map<int, std::shared_ptr<RTSPSession<>>> sessions_;

    utils::NonBlockingQueue<std::pair<int, rtsp::RTSPRequest>> cmd_queue_;

    // Pacer thread pool to avoid latency for multiple stream.
    utils::ThreadPool<PacerWorkers> pacer_pool_;

    std::jthread dispatcher_thread_;
    // orchestrate to timer fd
    std::jthread pacer_thread_;
    std::jthread logic_thread_;
};

using DefaultReactor = Reactor<4>;
extern template class Reactor<4>;

} // namespace rtspserver::server

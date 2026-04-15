#pragma once

#include "rtspserver/utils/BlockingQueue.hpp"

#include <array>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace rtspserver::utils {

/**
 * @brief ThreadPool with a fixed number of worker threads specified by the template parameter.
 *
 * @tparam N
 */
template <std::size_t N>
class ThreadPool {
    static_assert(N >= 1, "ThreadPool<N>: N must be at least 1");

public:
    ThreadPool()
        : tasks_()
    {
        for (std::size_t i = 0; i < N; ++i) {
            workers_[i] = std::thread(&ThreadPool::workerLoop, this);
        }
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Submit a callable with optional arguments.
     * Returns std::future<ReturnType>.  The future holds any exception thrown
     *
     * @tparam F
     * @tparam Args
     * @param f
     * @param args
     * @return std::future<std::invoke_result_t<F, Args...>>
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        if (tasks_.is_closed()) {
            throw std::runtime_error("ThreadPool: submit on shut-down pool");
        }

        // Bind arguments into a zero-argument callable, then wrap in packaged_task
        // so the future propagates return values and exceptions.
        auto task = std::make_shared<std::packaged_task<R()>>(
            [fn = std::forward<F>(f),
                ... captured = std::forward<Args>(args)]() mutable -> R {
                return std::invoke(std::forward<decltype(fn)>(fn),
                    std::forward<decltype(captured)>(captured)...);
            });

        std::future<R> fut = task->get_future();

        tasks_.push([task = std::move(task)] { (*task)(); });

        return fut;
    }

    /**
     * @brief Finish all enqueued tasks, then stop all worker threads.
     * Safe to call multiple times (idempotent after first call).
     *
     */
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(shutdown_mutex_);
            if (shutdown_done_)
                return;
            shutdown_done_ = true;
        }

        // Signal the queue: no new pushes; drain existing items, then stop.
        tasks_.close();

        for (auto& w : workers_) {
            if (w.joinable())
                w.join();
        }
    }

    /**
     * @brief Return the number of worker threads in the pool.
     *
     * @return constexpr std::size_t
     */
    static constexpr std::size_t thread_count() noexcept { return N; }

private:
    /**
     * @brief Worker thread loop: pop tasks and execute them until the queue is closed and empty.
     *
     */
    void workerLoop()
    {
        while (true) {
            auto task = tasks_.pop();
            if (!task)
                break;
            (*task)();
        }
    }

    BlockingQueue<std::function<void()>> tasks_;
    std::array<std::thread, N> workers_;

    std::mutex shutdown_mutex_;
    bool shutdown_done_ { false };
};

} // namespace rtspserver::utils

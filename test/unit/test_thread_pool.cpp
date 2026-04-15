#include "rtspserver/utils/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <vector>

using namespace rtspserver::utils;
using namespace std::chrono_literals;

TEST(ThreadPool, SubmitReturnsCorrectValue)
{
    ThreadPool<2> pool;
    auto fut = pool.submit([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(ThreadPool, SubmitWithArgs)
{
    ThreadPool<2> pool;
    auto fut = pool.submit([](int a, int b) { return a + b; }, 10, 20);
    EXPECT_EQ(fut.get(), 30);
}

TEST(ThreadPool, SubmitVoidTask)
{
    ThreadPool<2> pool;
    std::atomic<int> counter { 0 };
    auto fut = pool.submit([&counter] { counter.fetch_add(1); });
    fut.get();
    EXPECT_EQ(counter.load(), 1);
}

TEST(ThreadPool, ExceptionPropagatesViaFuture)
{
    ThreadPool<2> pool;
    auto fut = pool.submit([]() -> int { throw std::runtime_error("oops"); });
    EXPECT_THROW(fut.get(), std::runtime_error);
}

TEST(ThreadPool, ThreadCount)
{
    static_assert(ThreadPool<1>::thread_count() == 1);
    static_assert(ThreadPool<4>::thread_count() == 4);
    static_assert(ThreadPool<8>::thread_count() == 8);
}

TEST(ThreadPool, NTasksRunConcurrentlyUpToN)
{
    constexpr std::size_t N = 4;
    ThreadPool<N> pool;

    std::atomic<int> in_flight { 0 };
    std::atomic<int> peak { 0 };

    std::vector<std::future<void>> futs;
    futs.reserve(N);

    std::mutex go_mtx;
    std::condition_variable go_cv;
    bool go { false };

    // All N tasks start, count in-flight, then wait for the go signal.
    for (std::size_t i = 0; i < N; ++i) {
        futs.push_back(pool.submit([&] {
            int cur = in_flight.fetch_add(1) + 1;
            int old = peak.load();
            while (old < cur && !peak.compare_exchange_weak(old, cur)) { }

            std::unique_lock<std::mutex> lk(go_mtx);
            go_cv.wait(lk, [&] { return go; });

            in_flight.fetch_sub(1);
        }));
    }

    // Let them all pile up, then release.
    std::this_thread::sleep_for(40ms);
    {
        std::lock_guard<std::mutex> lk(go_mtx);
        go = true;
    }
    go_cv.notify_all();

    for (auto& f : futs)
        f.get();

    EXPECT_EQ(peak.load(), static_cast<int>(N));
}

TEST(ThreadPool, SumOverManyTasks)
{
    constexpr int kTasks = 1000;
    ThreadPool<4> pool;

    std::atomic<long long> total { 0 };
    std::vector<std::future<void>> futs;
    futs.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futs.push_back(pool.submit([&total, i] {
            total.fetch_add(i, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futs)
        f.get();

    long long expected = static_cast<long long>(kTasks - 1) * kTasks / 2;
    EXPECT_EQ(total.load(), expected);
}

TEST(ThreadPool, ShutdownIdempotent)
{
    ThreadPool<2> pool;
    pool.submit([] { std::this_thread::sleep_for(5ms); });
    pool.shutdown();
    EXPECT_NO_THROW(pool.shutdown()); // second call must not throw / crash
}

TEST(ThreadPool, SubmitAfterShutdownThrows)
{
    ThreadPool<2> pool;
    pool.shutdown();
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

TEST(ThreadPool, DestructorDrainsRemainingTasks)
{
    std::atomic<int> done { 0 };
    {
        ThreadPool<2> pool;
        for (int i = 0; i < 10; ++i)
            pool.submit([&done] { done.fetch_add(1); });
        // destructor calls shutdown() which joins workers after draining
    }
    EXPECT_EQ(done.load(), 10);
}

TEST(ThreadPool, SingleWorkerSerializesAllTasks)
{
    ThreadPool<1> pool;
    std::vector<int> order;
    std::mutex mtx;

    std::vector<std::future<void>> futs;
    for (int i = 0; i < 5; ++i) {
        futs.push_back(pool.submit([&order, &mtx, i] {
            std::lock_guard<std::mutex> lk(mtx);
            order.push_back(i);
        }));
    }
    for (auto& f : futs)
        f.get();

    // Single worker means tasks run in submission order.
    EXPECT_EQ(order, (std::vector<int> { 0, 1, 2, 3, 4 }));
}

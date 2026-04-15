#include "rtspserver/utils/BlockingQueue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <thread>
#include <vector>

using namespace rtspserver::utils;
using namespace std::chrono_literals;

TEST(BlockingQueue, PushPopSingleThread)
{
    BlockingQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.pop(), 1);
    EXPECT_EQ(q.pop(), 2);
    EXPECT_EQ(q.pop(), 3);
    EXPECT_TRUE(q.empty());
}

TEST(BlockingQueue, TryPopEmpty)
{
    BlockingQueue<int> q;
    EXPECT_EQ(q.try_pop(), std::nullopt);
}

TEST(BlockingQueue, PopForTimeout)
{
    BlockingQueue<int> q;
    auto result = q.pop_for(20ms);
    EXPECT_EQ(result, std::nullopt);
}

TEST(BlockingQueue, PopForReceivesValue)
{
    BlockingQueue<int> q;
    std::thread producer([&] {
        std::this_thread::sleep_for(10ms);
        q.push(42);
    });
    auto result = q.pop_for(200ms);
    producer.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(BlockingQueue, PopOnClosedEmptyReturnsNullopt)
{
    BlockingQueue<int> q;
    q.close();
    EXPECT_EQ(q.pop(), std::nullopt);
}

TEST(BlockingQueue, PopDrainsBeforeClosedSignal)
{
    BlockingQueue<int> q;
    q.push(7);
    q.push(8);
    q.close();
    EXPECT_EQ(q.pop(), 7);
    EXPECT_EQ(q.pop(), 8);
    EXPECT_EQ(q.pop(), std::nullopt); // drained, now signals closed
}

TEST(BlockingQueue, PushAfterCloseThrows)
{
    BlockingQueue<int> q;
    q.close();
    EXPECT_THROW(q.push(1), std::runtime_error);
}

TEST(BlockingQueue, TryPushAfterCloseReturnsFalse)
{
    BlockingQueue<int> q;
    q.close();
    EXPECT_FALSE(q.try_push(1));
}

TEST(BlockingQueue, BlockedPopWokenByClose)
{
    BlockingQueue<int> q;
    std::future<std::optional<int>> fut = std::async(std::launch::async, [&] {
        return q.pop(); // will block until close()
    });
    std::this_thread::sleep_for(20ms);
    q.close();
    auto result = fut.get();
    EXPECT_EQ(result, std::nullopt);
}

TEST(BlockingQueueBounded, TryPushToBoundedFull)
{
    BlockingQueue<int, 2> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_FALSE(q.try_push(3)); // full
    EXPECT_EQ(q.size(), 2u);
}

TEST(BlockingQueueBounded, PushBlocksWhenFull)
{
    BlockingQueue<int, 1> q;
    q.push(1);

    std::atomic<bool> pushed { false };
    std::thread producer([&] {
        q.push(2); // blocks until consumer pops
        pushed.store(true);
    });

    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(pushed.load()); // still blocked
    q.pop(); // unblock producer

    producer.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(q.pop(), 2);
}

TEST(BlockingQueueBounded, PushForTimesOutWhenFull)
{
    BlockingQueue<int, 1> q;
    q.push(1);
    EXPECT_FALSE(q.push_for(2, 20ms));
}

TEST(BlockingQueueBounded, CapacityConstexpr)
{
    using Q = BlockingQueue<int, 8>;
    EXPECT_EQ(Q::capacity(), 8u);
}

TEST(BlockingQueue, MPSCAllItemsDelivered)
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 500;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> q;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i)
                q.push(p * kPerProducer + i);
        });
    }

    std::vector<int> received;
    received.reserve(kTotal);

    // Consumer thread
    std::thread consumer([&] {
        while (static_cast<int>(received.size()) < kTotal) {
            if (auto v = q.try_pop()) {
                received.push_back(*v);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers)
        t.join();
    consumer.join();

    EXPECT_EQ(static_cast<int>(received.size()), kTotal);
    // All values 0..kTotal-1 must appear exactly once
    std::sort(received.begin(), received.end());
    for (int i = 0; i < kTotal; ++i)
        EXPECT_EQ(received[i], i);
}

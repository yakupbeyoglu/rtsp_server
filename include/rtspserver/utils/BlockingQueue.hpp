#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// BlockingQueue<T> — thread-safe, optionally bounded FIFO queue.
//
// Multiple producers and multiple consumers are safe.
//
// Blocking behaviour:
//   push()     – blocks when the queue is full (bounded) until space is
//                available or the queue is closed.
//   pop()      – blocks until an item is available or the queue is closed.
//
// Non-blocking / timed variants return std::optional<T> (empty on timeout
// or closed queue).
//
// Shutdown:
//   close()     – wakes all blocked push()/pop() callers; once closed every
//                 subsequent push fails and every pop returns std::nullopt
//                 once the queue is drained.
//
// Template parameters:
//   T          – the element type (must be movable)
//   Capacity   – maximum queue depth (0 = unbounded, default)
// ─────────────────────────────────────────────────────────────────────────────

namespace rtspserver::utils {

/**
 * @brief Thread Safe Blocking Queue implementation with optional capacity limit.
 *
 * @tparam T
 * @tparam Capacity
 */
template <typename T, std::size_t Capacity = 0>
class BlockingQueue {
public:
    BlockingQueue() = default;
    ~BlockingQueue() { close(); }

    // Delete Copy Constructor and Copy Assignment Operator to prevent copying of the queue
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // Delete move constructor and move assignment operator
    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    /**
     * @brief Push Item to queue
     *
     * @param value
     */
    void push(T value)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (closed_)
            throw std::runtime_error("BlockingQueue: push on closed queue");
        not_full_.wait(lk, [this] { return !is_full() || closed_; });
        if (closed_)
            throw std::runtime_error("BlockingQueue: push on closed queue");
        queue_.push_back(std::move(value));
        lk.unlock();
        not_empty_.notify_one();
    }

    /**
     * @brief Try to push with in a timeout,
     * If Queue is full or closed return false
     *
     * @tparam Rep
     * @tparam Period
     * @param value
     * @param timeout
     * @return true
     * @return false
     */
    template <class Rep, class Period>
    bool push_for(T value, const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (closed_)
            return false;
        bool ok = not_full_.wait_for(lk, timeout,
            [this] { return !is_full() || closed_; });
        if (!ok || closed_)
            return false;
        queue_.push_back(std::move(value));
        lk.unlock();
        not_empty_.notify_one();
        return true;
    }

    /**
     * @brief Non blocking Push
     * IF Queue is full or closed return false immediately
     *
     * @param value
     * @return true
     * @return false
     */
    bool try_push(T value)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (closed_ || is_full())
            return false;
        queue_.push_back(std::move(value));
        lk.unlock();
        not_empty_.notify_one();
        return true;
    }

    /**
     * @brief Pop an item from the queue. Blocks until an item is available or the queue is closed.
     *
     * @return std::optional<T>
     */
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        not_empty_.wait(lk, [this] { return !queue_.empty() || closed_; });
        return dequeue(lk);
    }

    /**
     * @brief Timed pop.  Returns std::nullopt on timeout or closed+empty.
     *
     * @tparam Rep
     * @tparam Period
     * @param timeout
     * @return std::optional<T>
     */
    template <class Rep, class Period>
    std::optional<T> pop_for(const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        not_empty_.wait_for(lk, timeout,
            [this] { return !queue_.empty() || closed_; });
        return dequeue(lk);
    }

    /**
     * @brief Non-blocking pop.  Returns std::nullopt if the queue is empty or closed.
     *
     * @return std::optional<T>
     */
    std::optional<T> try_pop()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return dequeue(lk);
    }

    /**
     * @brief Close the queue.
     *
     */
    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /**
     * @brief Check Is Closed or not
     *
     * @return true
     * @return false
     */
    bool is_closed() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return closed_;
    }

    /**
     * @brief Get the Current sizse of queue
     *
     * @return std::size_t
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    /**
     * @brief Check if the queue is empty
     *
     * @return true
     * @return false
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Capacity of the queue
     *
     * @return constexpr std::size_t
     */
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    /**
     * @brief Check if the queue is full (only relevant for bounded queues).
     *
     * @return true
     * @return false
     */
    bool is_full() const noexcept
    {
        if constexpr (Capacity == 0)
            return false; // unbounded
        return queue_.size() >= Capacity;
    }

    /**
     * @brief Dequeue an item. Caller must hold the lock. Returns std::nullopt if the queue is empty.
     *
     */
    std::optional<T> dequeue(std::unique_lock<std::mutex>&)
    {
        if (queue_.empty())
            return std::nullopt;
        T v = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return v;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    bool closed_ { false };
};

} // namespace rtspserver::utils

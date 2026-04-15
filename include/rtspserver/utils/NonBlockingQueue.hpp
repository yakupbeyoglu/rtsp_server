#pragma once

#include <deque>
#include <mutex>
#include <optional>

namespace rtspserver::utils {

/**
 * @brief Non-blocking thread-safe FIFO queue.
 *
 * @tparam T The type of elements stored in the queue.
 */
template <typename T>
class NonBlockingQueue {
public:
    /**
     * @brief Push an item to the back of the queue.
     *
     * @param item
     */
    void push(T item)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(std::move(item));
    }

    /**
     * @brief Try to pop an item from the front of the queue.
     * Returns std::nullopt if the queue is empty.
     *
     * @return std::optional<T>
     */
    std::optional<T> try_pop()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    /**
     * @brief Check if the queue is empty
     *
     * @return true
     * @return false
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.empty();
    }

    /**
     * @brief Get the Current size of queue
     *
     * @return std::size_t
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }

private:
    mutable std::mutex mtx_;
    std::deque<T> queue_;
};

} // namespace rtspserver::utils

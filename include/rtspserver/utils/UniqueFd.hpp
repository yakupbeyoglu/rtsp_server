#pragma once

#include <unistd.h>

namespace rtspserver::utils {

/**
 * @brief RAII wrapper for a POSIX file descriptor.
 *
 */
class UniqueFd {
public:
    static constexpr int kInvalid = -1;

    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept
        : fd_(fd)
    {
    }

    ~UniqueFd() { close(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& o) noexcept
        : fd_(o.release())
    {
    }
    UniqueFd& operator=(UniqueFd&& o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] int release() noexcept
    {
        int tmp = fd_;
        fd_ = kInvalid;
        return tmp;
    }

    void reset(int fd = kInvalid) noexcept
    {
        close();
        fd_ = fd;
    }

private:
    void close() noexcept
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = kInvalid;
        }
    }

    int fd_ { kInvalid };
};

} // namespace rtspserver::utils

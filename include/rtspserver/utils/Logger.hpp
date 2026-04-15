#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Logger – selectable backend.
//
// When the project is built with -DRTSP_USE_SPDLOG=ON and spdlog is found,
// CMake defines RTSP_USE_SPDLOG as a PUBLIC compile definition on rtsp_core.
// In that case the LOG_* macros delegate directly to spdlog's thread-safe,
// lock-free API.
//
// Otherwise the built-in Logger singleton (thread-safe, localtime_r,
// stdout/stderr split by level) is used.  The LOG_* macro interface is
// identical in both cases, so no call-site changes are ever needed.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef RTSP_USE_SPDLOG
// ─────────────────────────────────────────────────────────────────────────────
// spdlog backend
// ─────────────────────────────────────────────────────────────────────────────

#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace rtspserver::utils {

// LogLevel mirrors the built-in enum so existing setLevel() call sites compile
// without modification.
enum class LogLevel { DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3 };

// Thin shim that lets main.cpp call Logger::instance().setLevel(LogLevel::DEBUG)
// exactly as before; it simply forwards to spdlog's global level.
class Logger {
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    void setLevel(LogLevel level)
    {
        spdlog::set_level(toSpdLevel(level));
    }

    LogLevel getLevel() const
    {
        return fromSpdLevel(spdlog::get_level());
    }

    // Variadic log() kept for any direct callers.
    template <typename... Args>
    void log(LogLevel level, Args&&... args)
    {
        std::string msg;
        msg.reserve(128);
        ((msg += toStr(std::forward<Args>(args))), ...);
        spdlog::log(toSpdLevel(level), "{}", msg);
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static spdlog::level::level_enum toSpdLevel(LogLevel l) noexcept
    {
        switch (l) {
        case LogLevel::DEBUG:
            return spdlog::level::debug;
        case LogLevel::INFO:
            return spdlog::level::info;
        case LogLevel::WARN:
            return spdlog::level::warn;
        case LogLevel::ERROR:
            return spdlog::level::err;
        }
        return spdlog::level::info;
    }

    static LogLevel fromSpdLevel(spdlog::level::level_enum l) noexcept
    {
        switch (l) {
        case spdlog::level::debug:
            return LogLevel::DEBUG;
        case spdlog::level::warn:
            return LogLevel::WARN;
        case spdlog::level::err:
        case spdlog::level::critical:
            return LogLevel::ERROR;
        default:
            return LogLevel::INFO;
        }
    }

    template <typename T>
    static std::string toStr(T&& v)
    {
        if constexpr (std::is_convertible_v<T, std::string_view>) {
            return std::string(std::string_view(std::forward<T>(v)));
        } else {
            std::ostringstream oss;
            oss << std::forward<T>(v);
            return oss.str();
        }
    }
};

} // namespace rtspserver::utils

#define LOG_DEBUG(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::ERROR, __VA_ARGS__)

#else
// ─────────────────────────────────────────────────────────────────────────────
// Built-in backend (no external dependency)
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace rtspserver::utils {

enum class LogLevel { DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3 };

class Logger {
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    void setLevel(LogLevel level) { min_level_ = level; }
    LogLevel getLevel() const { return min_level_; }

    template <typename... Args>
    void log(LogLevel level, Args&&... args)
    {
        if (level < min_level_)
            return;

        std::ostringstream oss;
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
            % 1000;

        // localtime_r is thread-safe (POSIX); std::localtime is not.
        std::tm tm_buf {};
        ::localtime_r(&t, &tm_buf);
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << levelStr(level) << "] ";
        (oss << ... << std::forward<Args>(args));

        std::lock_guard<std::mutex> lock(mutex_);
        (level >= LogLevel::WARN ? std::cerr : std::cout) << oss.str() << '\n';
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel min_level_ { LogLevel::INFO };
    std::mutex mutex_;

    static const char* levelStr(LogLevel l) noexcept
    {
        switch (l) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO ";
        case LogLevel::WARN:
            return "WARN ";
        case LogLevel::ERROR:
            return "ERROR";
        }
        return "?????";
    }
};

} // namespace rtspserver::utils

#define LOG_DEBUG(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) ::rtspserver::utils::Logger::instance().log(::rtspserver::utils::LogLevel::ERROR, __VA_ARGS__)

#endif // RTSP_USE_SPDLOG

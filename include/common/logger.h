#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>

namespace dfs {

enum class LogLevel {
    DEBUG_LVL,
    INFO_LVL,
    WARN_LVL,
    ERROR_LVL
};

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setLogFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_stream_.open(path, std::ios::app);
    }

    void log(LogLevel level, const std::string& module, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm time_info;
#if defined(_WIN32)
        localtime_s(&time_info, &in_time_t);
#else
        localtime_r(&in_time_t, &time_info);
#endif

        std::ostringstream ss;
        ss << "[" << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S") << "."
           << std::setw(3) << std::setfill('0') << ms.count() << "] ["
           << levelToString(level) << "] [" << module << "] " << message;

        std::string line = ss.str();
        std::cout << line << std::endl;
        if (file_stream_.is_open()) {
            file_stream_ << line << std::endl;
        }
    }

private:
    Logger() = default;
    ~Logger() {
        if (file_stream_.is_open()) file_stream_.close();
    }

    std::string levelToString(LogLevel l) {
        switch (l) {
            case LogLevel::DEBUG_LVL: return "DEBUG";
            case LogLevel::INFO_LVL: return "INFO";
            case LogLevel::WARN_LVL: return "WARN";
            case LogLevel::ERROR_LVL: return "ERROR";
            default: return "LOG";
        }
    }

    std::mutex mutex_;
    std::ofstream file_stream_;
};

#define LOG_DEBUG(mod, msg) dfs::Logger::instance().log(dfs::LogLevel::DEBUG_LVL, mod, msg)
#define LOG_INFO(mod, msg) dfs::Logger::instance().log(dfs::LogLevel::INFO_LVL, mod, msg)
#define LOG_WARN(mod, msg) dfs::Logger::instance().log(dfs::LogLevel::WARN_LVL, mod, msg)
#define LOG_ERROR(mod, msg) dfs::Logger::instance().log(dfs::LogLevel::ERROR_LVL, mod, msg)

} // namespace dfs

#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace AlsaAVR {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
private:
    std::ofstream log_file_;
    LogLevel min_level_ = LogLevel::INFO;
    std::mutex mutex_;
    bool file_enabled_ = false;

    static const char* level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }

    static LogLevel string_to_level(const std::string& str) {
        if (str == "DEBUG") return LogLevel::DEBUG;
        if (str == "INFO") return LogLevel::INFO;
        if (str == "WARN") return LogLevel::WARN;
        if (str == "ERROR") return LogLevel::ERROR;
        return LogLevel::INFO;
    }

    std::string get_timestamp() {
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
        return std::string(buffer);
    }

    void write_log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ostringstream oss;
        oss << "[" << get_timestamp() << "] "
            << std::setw(5) << std::left << level_to_string(level) << ": "
            << message;

        std::string formatted = oss.str();

        // Write to file if enabled
        if (file_enabled_ && log_file_.is_open()) {
            log_file_ << formatted << std::endl;
            log_file_.flush();
        }

        // Always write errors to stderr
        if (level == LogLevel::ERROR) {
            std::cerr << formatted << std::endl;
        }
    }

public:
    void init(const std::string& log_file_path, const std::string& log_level_str) {
        std::lock_guard<std::mutex> lock(mutex_);

        min_level_ = string_to_level(log_level_str);

        if (!log_file_path.empty()) {
            log_file_.open(log_file_path, std::ios::app);
            if (log_file_.is_open()) {
                file_enabled_ = true;
                // Only log initialization at DEBUG level
                if (min_level_ == LogLevel::DEBUG) {
                    log_file_ << "[" << get_timestamp() << "] DEBUG: Logger initialized (level: "
                             << log_level_str << ")" << std::endl;
                    log_file_.flush();
                }
            } else {
                std::cerr << "Warning: Could not open log file: " << log_file_path << std::endl;
            }
        }
    }

    void debug(const std::string& message) {
        if (min_level_ <= LogLevel::DEBUG) {
            write_log(LogLevel::DEBUG, message);
        }
    }

    void info(const std::string& message) {
        if (min_level_ <= LogLevel::INFO) {
            write_log(LogLevel::INFO, message);
        }
    }

    void warn(const std::string& message) {
        if (min_level_ <= LogLevel::WARN) {
            write_log(LogLevel::WARN, message);
        }
    }

    void error(const std::string& message) {
        if (min_level_ <= LogLevel::ERROR) {
            write_log(LogLevel::ERROR, message);
        }
    }

    ~Logger() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (log_file_.is_open()) {
            // Only log shutdown at DEBUG level
            if (min_level_ == LogLevel::DEBUG) {
                log_file_ << "[" << get_timestamp() << "] DEBUG: Logger shutting down" << std::endl;
            }
            log_file_.close();
        }
    }
};

} // namespace AlsaAVR

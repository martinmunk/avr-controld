#include "../../include/backend.hpp"
#include "../../include/logger.hpp"
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <regex>
#include <chrono>
#include <memory>
#include <mutex>
#include <map>

namespace AlsaAVR {

// ============================================================================
// NADConnection: Owns the serial FD and all receiver state
// ============================================================================
class NADConnection {
private:
    int fd_ = -1;
    std::string device_;
    std::mutex mutex_;

    // Receiver state
    int current_volume_ = -1;  // Volume in 0-100 scale
    bool current_mute_ = false;
    bool current_power_ = false;

    Logger logger_;

    // NAD volume ranges from -90dB to 0dB
    static constexpr int NAD_MIN_VOLUME_DB = -90;
    static constexpr int NAD_MAX_VOLUME_DB = 0;

    bool open_serial() {
        fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd_ < 0) {
            logger_.error("Failed to open serial device: " + device_ + " (errno=" + std::to_string(errno) + ")");
            return false;
        }

        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) {
            logger_.error("Failed to get serial attributes");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Set 115200 baud, 8N1
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_iflag &= ~IGNBRK;
        tty.c_lflag = 0;
        tty.c_oflag = 0;
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 5;

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~(PARENB | PARODD);
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Flush any stale data
        tcflush(fd_, TCIOFLUSH);

        return true;
    }

    bool send_command(const std::string& cmd) {
        if (fd_ < 0) return false;

        std::string full_cmd = "\r\n" + cmd + "\r";
        ssize_t written = write(fd_, full_cmd.c_str(), full_cmd.length());
        if (written != static_cast<ssize_t>(full_cmd.length())) {
            logger_.error("Failed to send command: " + cmd);
            return false;
        }

        logger_.debug("Sent command: " + cmd);
        return true;
    }

    std::string read_response(int timeout_ms = 500) {
        if (fd_ < 0) return "";

        std::string result;
        char buffer[256];
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            fd_set readfds;
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 50000;

            FD_ZERO(&readfds);
            FD_SET(fd_, &readfds);

            int ret = select(fd_ + 1, &readfds, nullptr, nullptr, &tv);
            if (ret > 0) {
                ssize_t n = read(fd_, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    result += buffer;

                    if (result.find('\r') != std::string::npos || result.find('\n') != std::string::npos) {
                        break;
                    }
                }
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= timeout_ms) break;
        }

        return result;
    }

    int parse_volume_response(const std::string& response) {
        std::regex vol_regex(R"(Main\.Volume=(-?\d+))");
        std::smatch match;
        if (std::regex_search(response, match, vol_regex) && match.size() > 1) {
            try {
                return std::stoi(match[1].str());
            } catch (...) {}
        }
        return NAD_MIN_VOLUME_DB;
    }

    int scale_to_db(int level) {
        return NAD_MIN_VOLUME_DB + (level * (NAD_MAX_VOLUME_DB - NAD_MIN_VOLUME_DB)) / 100;
    }

    int db_to_scale(int db) {
        if (db < NAD_MIN_VOLUME_DB) db = NAD_MIN_VOLUME_DB;
        if (db > NAD_MAX_VOLUME_DB) db = NAD_MAX_VOLUME_DB;
        return ((db - NAD_MIN_VOLUME_DB) * 100) / (NAD_MAX_VOLUME_DB - NAD_MIN_VOLUME_DB);
    }

    void ensure_powered_on() {
        if (!current_power_) {
            logger_.info("Receiver is off, powering on automatically");
            if (send_command("Main.Power=On")) {
                current_power_ = true;
                usleep(500000);
            }
        }
    }

public:
    NADConnection(const std::string& device, const std::string& log_file, const std::string& log_level)
        : device_(device) {
        logger_.init(log_file, log_level);

        logger_.info("Creating new NAD connection to " + device);

        if (!open_serial()) {
            logger_.error("Failed to open serial connection");
            return;
        }

        logger_.info("Connected successfully");
        logger_.info("Probing initial receiver state");

        // Query initial state
        if (send_command("Main.Power?")) {
            std::string response = read_response(500);
            current_power_ = (response.find("Main.Power=On") != std::string::npos);
            logger_.info("Probed initial power: " + std::string(current_power_ ? "On" : "Off"));
        }

        if (send_command("Main.Volume?")) {
            std::string response = read_response(1000);
            int db = parse_volume_response(response);
            current_volume_ = db_to_scale(db);
            logger_.info("Probed initial volume: " + std::to_string(current_volume_) + "% (" + std::to_string(db) + "dB)");
        }

        if (send_command("Main.Mute?")) {
            std::string response = read_response(500);
            current_mute_ = (response.find("Main.Mute=On") != std::string::npos);
            logger_.info("Probed initial mute: " + std::string(current_mute_ ? "On" : "Off"));
        }
    }

    ~NADConnection() {
        if (fd_ >= 0) {
            logger_.debug("Closing NAD connection");
            ::close(fd_);
        }
    }

    bool is_connected() const {
        return fd_ >= 0;
    }

    bool set_volume_absolute(int level) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < 0 || level > 100) return false;

        ensure_powered_on();

        int db = scale_to_db(level);
        std::ostringstream cmd;
        cmd << "Main.Volume=" << db;

        if (send_command(cmd.str())) {
            std::string response = read_response(500);
            int actual_db = parse_volume_response(response);
            current_volume_ = db_to_scale(actual_db);
            logger_.debug("Set volume to " + std::to_string(level) + "% (" + std::to_string(db) + "dB)");
            return true;
        }
        return false;
    }

    bool volume_up() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_powered_on();

        if (send_command("Main.Volume+")) {
            std::string response = read_response(500);
            int db = parse_volume_response(response);
            current_volume_ = db_to_scale(db);
            logger_.debug("Volume up -> " + std::to_string(current_volume_) + "%");
            return true;
        }
        return false;
    }

    bool volume_down() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_powered_on();

        if (send_command("Main.Volume-")) {
            std::string response = read_response(500);
            int db = parse_volume_response(response);
            current_volume_ = db_to_scale(db);
            logger_.debug("Volume down -> " + std::to_string(current_volume_) + "%");
            return true;
        }
        return false;
    }

    int get_volume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_command("Main.Volume?")) {
            std::string response = read_response(1000);
            int db = parse_volume_response(response);
            current_volume_ = db_to_scale(db);
        }
        return current_volume_;
    }

    bool set_mute(bool muted) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_powered_on();

        std::string cmd = muted ? "Main.Mute=On" : "Main.Mute=Off";
        if (send_command(cmd)) {
            current_mute_ = muted;
            logger_.debug(muted ? "Muted" : "Unmuted");
            return true;
        }
        return false;
    }

    bool get_mute() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_command("Main.Mute?")) {
            std::string response = read_response(500);
            current_mute_ = (response.find("Main.Mute=On") != std::string::npos);
        }
        return current_mute_;
    }

    bool set_power(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string cmd = on ? "Main.Power=On" : "Main.Power=Off";
        if (send_command(cmd)) {
            current_power_ = on;
            logger_.info(on ? "Powered on" : "Powered off");
            return true;
        }
        return false;
    }

    bool get_power() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_command("Main.Power?")) {
            std::string response = read_response(500);
            current_power_ = (response.find("Main.Power=On") != std::string::npos);
        }
        return current_power_;
    }

    int get_cached_volume() const { return current_volume_; }
    bool get_cached_mute() const { return current_mute_; }
    bool get_cached_power() const { return current_power_; }
};

// ============================================================================
// NADConnectionPool: Manages shared NADConnection instances
// ============================================================================
class NADConnectionPool {
private:
    static std::map<std::string, std::shared_ptr<NADConnection>> connections_;
    static std::mutex mutex_;

public:
    static std::shared_ptr<NADConnection> get_connection(
        const std::string& device,
        const std::string& log_file,
        const std::string& log_level) {

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = connections_.find(device);
        if (it != connections_.end() && it->second && it->second->is_connected()) {
            return it->second;
        }

        // Create new connection
        auto conn = std::make_shared<NADConnection>(device, log_file, log_level);
        if (conn->is_connected()) {
            connections_[device] = conn;
            return conn;
        }

        return nullptr;
    }

    static void remove_connection(const std::string& device) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(device);
    }
};

// Static member definitions
std::map<std::string, std::shared_ptr<NADConnection>> NADConnectionPool::connections_;
std::mutex NADConnectionPool::mutex_;

// ============================================================================
// NADSerialBackend: Thin wrapper that delegates to shared NADConnection
// ============================================================================
class NADSerialBackend : public Backend {
private:
    std::shared_ptr<NADConnection> conn_;
    std::string device_;
    Logger logger_;

public:
    bool init(const std::map<std::string, std::string>& config) override {
        auto log_file_it = config.find("log_file");
        auto log_level_it = config.find("log_level");
        std::string log_file = log_file_it != config.end() ? log_file_it->second : "";
        std::string log_level = log_level_it != config.end() ? log_level_it->second : "INFO";
        logger_.init(log_file, log_level);

        logger_.debug("Initializing NAD Serial backend");

        auto it = config.find("device");
        if (it == config.end()) {
            logger_.error("No device specified in configuration");
            return false;
        }
        device_ = it->second;
        logger_.debug("Device: " + device_);

        // Get or create shared connection
        conn_ = NADConnectionPool::get_connection(device_, log_file, log_level);
        if (!conn_) {
            logger_.error("Failed to get connection from pool");
            return false;
        }

        logger_.debug("Backend initialized successfully");
        return true;
    }

    void close() override {
        logger_.debug("Backend closing (connection remains in pool)");
        conn_.reset();
    }

    BackendCaps capabilities() const override {
        return {
            .supports_absolute = true,
            .supports_query = true,
            .supports_mute = true,
            .supports_input_select = false,
            .supports_speaker_level = false,
            .min_volume = 0,
            .max_volume = 100
        };
    }

    bool set_volume_absolute(int level) override {
        return conn_ ? conn_->set_volume_absolute(level) : false;
    }

    bool volume_up() override {
        return conn_ ? conn_->volume_up() : false;
    }

    bool volume_down() override {
        return conn_ ? conn_->volume_down() : false;
    }

    std::optional<int> get_volume() override {
        return conn_ ? std::optional<int>(conn_->get_volume()) : std::nullopt;
    }

    bool set_mute(bool muted) override {
        return conn_ ? conn_->set_mute(muted) : false;
    }

    std::optional<bool> get_mute() override {
        return conn_ ? std::optional<bool>(conn_->get_mute()) : std::nullopt;
    }

    bool set_power(bool on) override {
        return conn_ ? conn_->set_power(on) : false;
    }

    std::optional<bool> get_power() override {
        return conn_ ? std::optional<bool>(conn_->get_power()) : std::nullopt;
    }

    std::string name() const override {
        return "NAD Serial";
    }

    bool is_connected() const override {
        return conn_ && conn_->is_connected();
    }

    bool reconnect() override {
        NADConnectionPool::remove_connection(device_);
        conn_.reset();
        return false;
    }
};

// Factory function
std::unique_ptr<Backend> create_nad_serial_backend() {
    return std::make_unique<NADSerialBackend>();
}

} // namespace AlsaAVR

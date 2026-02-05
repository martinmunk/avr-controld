#include "../../include/backend.hpp"
#include "../../include/logger.hpp"
#include "../../include/shared_state.hpp"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>

namespace AlsaAVR {

// Shared connection class that holds the serial port FD
class OpalumConnection {
private:
    int fd_ = -1;
    std::string device_;
    Logger logger_;
    time_t last_used_ = 0;
    time_t last_input_switch_ = 0;
    std::string current_input_;
    mutable std::mutex mutex_;

    bool open_serial() {
        fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd_ < 0) {
            logger_.error("Failed to open serial device: " + device_);
            return false;
        }

        struct termios tty;
        if (tcgetattr(fd_, &tty) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Set 115200 baud, 8N1
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;  // 8-bit chars
        tty.c_iflag &= ~IGNBRK;                       // disable break processing
        tty.c_lflag = 0;                              // no signaling chars, no echo, no canonical processing
        tty.c_oflag = 0;                              // no remapping, no delays
        tty.c_cc[VMIN]  = 0;                          // non-blocking read
        tty.c_cc[VTIME] = 5;                          // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);       // shut off xon/xoff ctrl
        tty.c_cflag |= (CLOCAL | CREAD);              // ignore modem controls, enable reading
        tty.c_cflag &= ~(PARENB | PARODD);            // no parity
        tty.c_cflag &= ~CSTOPB;                       // 1 stop bit
        tty.c_cflag &= ~CRTSCTS;                      // no flow control

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Flush any stale data in buffers
        tcflush(fd_, TCIOFLUSH);

        return true;
    }

public:
    OpalumConnection(const std::string& device, const std::string& log_file, const std::string& log_level)
        : device_(device) {
        logger_.init(log_file, log_level);
        logger_.info("Creating new Opalum connection to " + device);

        if (open_serial()) {
            logger_.info("Connected successfully");

            // Try to power on the device (may already be on)
            if (!send_command("pwr1")) {
                logger_.warn("Power-on command did not receive OK response (device may already be on)");
            } else {
                logger_.info("Device powered on");
            }
        }
    }

    ~OpalumConnection() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0) {
            logger_.info("Closing Opalum connection to " + device_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fd_ >= 0;
    }

    bool send_command(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (fd_ < 0) {
            logger_.error("Cannot send command, device not connected");
            return false;
        }

        std::string full_cmd = cmd + "\r";
        ssize_t written = write(fd_, full_cmd.c_str(), full_cmd.length());
        if (written != static_cast<ssize_t>(full_cmd.length())) {
            logger_.error("Failed to send command: " + cmd);
            return false;
        }

        logger_.debug("Sent command: " + cmd);
        last_used_ = time(nullptr);

        // Wait for response (OK or ER)
        char response[16] = {0};
        int attempts = 0;
        ssize_t total_read = 0;

        // Try reading for up to 500ms
        while (attempts < 10 && total_read < static_cast<ssize_t>(sizeof(response) - 1)) {
            usleep(50000); // 50ms between attempts
            ssize_t n = read(fd_, response + total_read, sizeof(response) - total_read - 1);
            if (n > 0) {
                total_read += n;
                response[total_read] = '\0';
                if (strstr(response, "OK") || strstr(response, "ER")) {
                    break;
                }
            }
            attempts++;
        }

        if (total_read <= 0) {
            logger_.warn("No response from device for command: " + cmd);
            return false;
        }

        bool has_ok = (strstr(response, "OK") != nullptr);
        bool has_error = (strstr(response, "ER") != nullptr);
        bool has_echo = (strstr(response, cmd.c_str()) != nullptr);

        bool success = (has_ok || has_echo) && !has_error;

        if (!success) {
            logger_.error("Command failed: " + cmd + " (response: " + std::string(response) + ")");
        } else {
            logger_.debug("Command succeeded: " + cmd + " (response: " + std::string(response) + ")");
        }
        return success;
    }

    void mark_used() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_used_ = time(nullptr);
    }

    // Input switching with cooldown (shared across all backends using this connection)
    bool should_switch_input(const std::string& desired_input, int cooldown_seconds = 10) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_input_ == desired_input) {
            return false;  // Already on correct input
        }
        time_t now = time(nullptr);
        return (now - last_input_switch_) >= cooldown_seconds;
    }

    void record_input_switch(const std::string& input) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_input_ = input;
        last_input_switch_ = time(nullptr);
    }

    std::string get_current_input() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_input_;
    }
};

// Connection pool - static members persist across plugin instances
class OpalumConnectionPool {
private:
    static std::map<std::string, std::shared_ptr<OpalumConnection>> connections_;
    static std::mutex mutex_;

public:
    static std::shared_ptr<OpalumConnection> acquire(
        const std::string& device,
        const std::string& log_file, const std::string& log_level) {

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = connections_.find(device);
        if (it != connections_.end() && it->second->is_connected()) {
            // Reuse existing connection
            it->second->mark_used();
            return it->second;
        }

        // Create new connection
        auto conn = std::make_shared<OpalumConnection>(device, log_file, log_level);
        if (conn->is_connected()) {
            connections_[device] = conn;
            return conn;
        }

        return nullptr;
    }
};

// Static initialization
std::map<std::string, std::shared_ptr<OpalumConnection>> OpalumConnectionPool::connections_;
std::mutex OpalumConnectionPool::mutex_;

// Backend implementation - thin wrapper around shared connection
class OpalumSerialBackend : public Backend {
private:
    std::shared_ptr<OpalumConnection> conn_;
    std::string input_to_switch_;  // Per-instance config for auto-switching

    // State tracking - nullopt means unknown (not set through plugin)
    std::optional<int> current_volume_;
    std::optional<bool> current_mute_;
    std::optional<bool> current_power_;
    std::optional<bool> current_standby_timeout_;

    // Opalum volume is 0x00-0x63 (0-99 decimal, 100 steps)
    static constexpr int OPALUM_MAX_VOLUME = 0x63;

    void switch_input_if_needed() {
        if (input_to_switch_.empty() || !conn_) return;

        if (conn_->should_switch_input(input_to_switch_, 10)) {
            if (conn_->send_command("src" + input_to_switch_)) {
                conn_->record_input_switch(input_to_switch_);
                usleep(100000); // Wait 100ms for input switch
            }
        }
    }

public:
    bool init(const std::map<std::string, std::string>& config) override {
        auto device_it = config.find("device");
        if (device_it == config.end()) {
            return false;
        }
        std::string device = device_it->second;

        std::string log_file = config.count("log_file") ? config.at("log_file") : "";
        std::string log_level = config.count("log_level") ? config.at("log_level") : "INFO";

        // Get optional input switching config
        auto input_it = config.find("input");
        if (input_it != config.end()) {
            input_to_switch_ = input_it->second;
        }

        // Acquire shared connection (creates if new)
        conn_ = OpalumConnectionPool::acquire(device, log_file, log_level);
        if (!conn_) return false;

        // Auto-switch input if configured
        if (!input_to_switch_.empty()) {
            std::string current = conn_->get_current_input();
            if (current != input_to_switch_) {
                switch_input_if_needed();
            }
        }

        return true;
    }

    void close() override {
        // Connection stays alive in pool (shared)
        conn_.reset();
    }

    BackendCaps capabilities() const override {
        return {
            .supports_absolute = true,
            .supports_query = false,  // No query command in protocol
            .supports_mute = false,   // Implemented via vol00
            .supports_input_select = true,
            .supports_speaker_level = false,
            .supports_standby_timeout = true,
            .min_volume = 0,
            .max_volume = 100
        };
    }

    bool set_volume_absolute(int level) override {
        if (!conn_ || level < 0 || level > 100) return false;

        // Switch to configured input if needed (with cooldown)
        switch_input_if_needed();

        // Convert 0-100 to 0x00-0x63
        int opalum_vol = (level * OPALUM_MAX_VOLUME) / 100;

        std::ostringstream cmd;
        cmd << "vol" << std::hex << std::setw(2) << std::setfill('0') << opalum_vol;

        if (conn_->send_command(cmd.str())) {
            current_volume_ = level;
            return true;
        }
        return false;
    }

    bool volume_up() override {
        if (!conn_) return false;
        if (conn_->send_command("vol+")) {
            if (current_volume_ && *current_volume_ < 100) {
                current_volume_ = *current_volume_ + 1;
            }
            return true;
        }
        return false;
    }

    bool volume_down() override {
        if (!conn_) return false;
        if (conn_->send_command("vol-")) {
            if (current_volume_ && *current_volume_ > 0) {
                current_volume_ = *current_volume_ - 1;
            }
            return true;
        }
        return false;
    }

    std::optional<int> get_volume() override {
        return current_volume_;  // Returns nullopt if never set
    }

    bool set_mute(bool muted) override {
        if (!conn_) return false;

        switch_input_if_needed();

        // Opalum doesn't have explicit mute, use vol00 for mute
        if (muted) {
            if (conn_->send_command("vol00")) {
                current_mute_ = true;
                return true;
            }
            return false;
        }
        current_mute_ = false;
        return true;
    }

    std::optional<bool> get_mute() override {
        return current_mute_;  // Returns nullopt if never set
    }

    bool set_power(bool on) override {
        if (!conn_) return false;
        if (conn_->send_command(on ? "pwr1" : "pwr0")) {
            current_power_ = on;
            return true;
        }
        return false;
    }

    std::optional<bool> get_power() override {
        return current_power_;  // Returns nullopt if never set
    }

    bool set_standby_timeout(bool enabled) override {
        if (!conn_) return false;
        if (conn_->send_command(enabled ? "sta1" : "sta0")) {
            current_standby_timeout_ = enabled;
            return true;
        }
        return false;
    }

    std::optional<bool> get_standby_timeout() override {
        return current_standby_timeout_;  // Returns nullopt if never set
    }

    std::string name() const override {
        return "Opalum RS232";
    }

    bool is_connected() const override {
        return conn_ && conn_->is_connected();
    }

    std::vector<std::string> get_available_inputs() override {
        return {"Toslink1", "Toslink2", "Coaxial", "Bluetooth", "Analog", "Mixer"};
    }

    bool set_input(const std::string& input) override {
        if (!conn_) return false;

        std::map<std::string, std::string> input_map = {
            {"Toslink1", "0"},
            {"Toslink2", "1"},
            {"Coaxial", "2"},
            {"Bluetooth", "3"},
            {"Analog", "4"},
            {"Mixer", "5"}
        };

        std::string input_num;
        auto it = input_map.find(input);
        if (it != input_map.end()) {
            input_num = it->second;
        } else {
            input_num = input;
        }

        if (conn_->send_command("src" + input_num)) {
            conn_->record_input_switch(input_num);
            usleep(100000);
            return true;
        }
        return false;
    }

    std::optional<std::string> get_current_input() override {
        if (!conn_) return std::nullopt;

        std::string input = conn_->get_current_input();
        if (input.empty()) return std::nullopt;  // Unknown state

        std::map<std::string, std::string> reverse_map = {
            {"0", "Toslink1"},
            {"1", "Toslink2"},
            {"2", "Coaxial"},
            {"3", "Bluetooth"},
            {"4", "Analog"},
            {"5", "Mixer"}
        };

        auto it = reverse_map.find(input);
        if (it != reverse_map.end()) {
            return it->second;
        }
        return input;
    }

    bool reconnect() override {
        // Pool handles reconnection automatically
        return conn_ && conn_->is_connected();
    }
};

// Factory function
std::unique_ptr<Backend> create_opalum_serial_backend() {
    return std::make_unique<OpalumSerialBackend>();
}

} // namespace AlsaAVR

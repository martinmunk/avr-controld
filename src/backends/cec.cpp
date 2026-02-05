#include "../../include/backend.hpp"
#include "../../include/logger.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>

namespace AlsaAVR {

// ============================================================================
// CECConnection: Manages CEC state and commands
// ============================================================================
class CECConnection {
private:
    int current_volume_ = 50;  // Start at 50% since we can't query
    bool current_mute_ = false;
    std::string cec_device_;
    mutable std::mutex mutex_;
    Logger logger_;

    bool send_cec_command(const std::string& cmd) {
        std::string full_cmd = "echo '" + cmd + "' | cec-client -s";
        if (!cec_device_.empty()) {
            full_cmd += " -d " + cec_device_;
        }
        full_cmd += " 2>/dev/null";

        logger_.debug("Executing CEC command: " + cmd);
        int ret = system(full_cmd.c_str());
        bool success = WIFEXITED(ret) && WEXITSTATUS(ret) == 0;

        if (success) {
            logger_.debug("CEC command succeeded");
        } else {
            logger_.error("CEC command failed: " + cmd);
        }
        return success;
    }

public:
    CECConnection(const std::string& device, int initial_volume,
                  const std::string& log_file, const std::string& log_level)
        : cec_device_(device), current_volume_(initial_volume) {
        logger_.init(log_file, log_level);
        logger_.info("Creating CEC connection" + (device.empty() ? "" : " (device: " + device + ")"));
        logger_.info("Initial volume: " + std::to_string(initial_volume) + "%");
    }

    ~CECConnection() {
        logger_.debug("CEC connection closing");
    }

    static bool check_cec_available() {
        int ret = system("which cec-client > /dev/null 2>&1");
        return WIFEXITED(ret) && WEXITSTATUS(ret) == 0;
    }

    bool is_connected() const {
        return check_cec_available();
    }

    bool set_volume_absolute(int level) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < 0 || level > 100) return false;

        int diff = level - current_volume_;
        if (diff == 0) return true;

        int steps = std::abs(diff);
        bool up = diff > 0;

        // Limit the number of steps to prevent long delays
        if (steps > 20) {
            logger_.warn("Volume change too large (" + std::to_string(steps) +
                        " steps), limiting to 20");
            steps = 20;
        }

        logger_.info("Setting volume from " + std::to_string(current_volume_) +
                    "% to " + std::to_string(level) + "% (" +
                    std::to_string(steps) + " steps " + (up ? "up" : "down") + ")");

        for (int i = 0; i < steps; i++) {
            if (up) {
                if (!send_cec_command("volup")) return false;
                current_volume_++;
            } else {
                if (!send_cec_command("voldown")) return false;
                current_volume_--;
            }
            usleep(100000); // 100ms between steps
        }

        current_volume_ = level;
        return true;
    }

    bool volume_up() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_cec_command("volup")) {
            if (current_volume_ < 100) {
                current_volume_++;
            }
            logger_.debug("Volume up -> " + std::to_string(current_volume_) + "%");
            return true;
        }
        return false;
    }

    bool volume_down() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_cec_command("voldown")) {
            if (current_volume_ > 0) {
                current_volume_--;
            }
            logger_.debug("Volume down -> " + std::to_string(current_volume_) + "%");
            return true;
        }
        return false;
    }

    int get_volume() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_volume_;
    }

    bool set_mute(bool muted) {
        std::lock_guard<std::mutex> lock(mutex_);
        // CEC mute is a toggle, not an absolute set
        if (current_mute_ != muted) {
            if (send_cec_command("mute")) {
                current_mute_ = muted;
                logger_.info(muted ? "Muted" : "Unmuted");
                return true;
            }
            return false;
        }
        return true;
    }

    bool get_mute() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_mute_;
    }

    bool set_power(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        logger_.info(on ? "Powering on audio system" : "Putting audio system on standby");
        if (on) {
            return send_cec_command("on 5");  // 5 = audio system
        } else {
            return send_cec_command("standby 5");
        }
    }
};

// ============================================================================
// CECConnectionPool: Manages shared CECConnection instances
// ============================================================================
class CECConnectionPool {
private:
    static std::map<std::string, std::shared_ptr<CECConnection>> connections_;
    static std::mutex mutex_;

    static std::string make_key(const std::string& device) {
        return device.empty() ? "default" : device;
    }

public:
    static std::shared_ptr<CECConnection> acquire(
        const std::string& device, int initial_volume,
        const std::string& log_file, const std::string& log_level) {

        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = make_key(device);

        auto it = connections_.find(key);
        if (it != connections_.end() && it->second && it->second->is_connected()) {
            return it->second;
        }

        // Check if cec-client is available before creating connection
        if (!CECConnection::check_cec_available()) {
            return nullptr;
        }

        auto conn = std::make_shared<CECConnection>(device, initial_volume, log_file, log_level);
        if (conn->is_connected()) {
            connections_[key] = conn;
            return conn;
        }

        return nullptr;
    }
};

// Static member definitions
std::map<std::string, std::shared_ptr<CECConnection>> CECConnectionPool::connections_;
std::mutex CECConnectionPool::mutex_;

// ============================================================================
// CECBackend: Thin wrapper that delegates to shared CECConnection
// ============================================================================
class CECBackend : public Backend {
private:
    std::shared_ptr<CECConnection> conn_;

public:
    bool init(const std::map<std::string, std::string>& config) override {
        std::string device;
        auto it = config.find("device");
        if (it != config.end()) {
            device = it->second;
        }

        int initial_volume = 50;
        auto vol_it = config.find("initial_volume");
        if (vol_it != config.end()) {
            try {
                initial_volume = std::stoi(vol_it->second);
            } catch (...) {
                initial_volume = 50;
            }
        }

        std::string log_file = config.count("log_file") ? config.at("log_file") : "";
        std::string log_level = config.count("log_level") ? config.at("log_level") : "INFO";

        conn_ = CECConnectionPool::acquire(device, initial_volume, log_file, log_level);
        return conn_ != nullptr;
    }

    void close() override {
        // Connection stays alive in pool (shared)
        conn_.reset();
    }

    BackendCaps capabilities() const override {
        return {
            .supports_absolute = false,  // CEC only supports incremental
            .supports_query = false,     // Can't query volume via CEC
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
        // CEC tracks volume internally but it's not queried from device
        // Return the tracked value (initialized from config)
        return conn_ ? std::optional<int>(conn_->get_volume()) : std::nullopt;
    }

    bool set_mute(bool muted) override {
        return conn_ ? conn_->set_mute(muted) : false;
    }

    std::optional<bool> get_mute() override {
        // CEC tracks mute internally but it's not queried from device
        return conn_ ? std::optional<bool>(conn_->get_mute()) : std::nullopt;
    }

    bool set_power(bool on) override {
        return conn_ ? conn_->set_power(on) : false;
    }

    std::string name() const override {
        return "CEC";
    }

    bool is_connected() const override {
        return conn_ && conn_->is_connected();
    }

    bool reconnect() override {
        // CEC doesn't maintain a persistent connection
        return conn_ && conn_->is_connected();
    }
};

// Factory function
std::unique_ptr<Backend> create_cec_backend() {
    return std::make_unique<CECBackend>();
}

} // namespace AlsaAVR

#include "../../include/backend.hpp"
#include "../../include/logger.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <errno.h>

namespace AlsaAVR {

// Denon volume is 00-98 (0.5 dB steps from -80dB to +18dB)
// Volume 00 = -80dB, Volume 80 = 0dB (reference), Volume 98 = +18dB
static constexpr int DENON_MAX_VOLUME = 98;

// Forward declaration
class DenonConnection;

// Connection class - owns the socket and all receiver state
class DenonConnection {
private:
    int fd_;
    std::string host_;
    int port_;
    bool connected_ = false;
    Logger logger_;

    // Receiver state (discovered once on construction, updated on commands)
    int volume_ = 50;
    bool mute_ = false;
    bool power_ = true;
    int max_volume_ = 98;
    std::string current_input_;
    std::string original_input_;  // Input when connection was created (restored on destruction)
    std::map<Speaker, int> speaker_levels_;
    std::map<std::string, std::string> available_inputs_;  // code -> display name
    time_t last_input_switch_ = 0;
    time_t last_used_;

    // Thread-safety
    mutable std::mutex state_mutex_;

    bool connect_socket() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Set connection timeout
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // Enable TCP keep-alive to maintain connection
        int keepalive = 1;
        int keepidle = 60;      // Start probes after 60 seconds of idle
        int keepintvl = 10;     // Send probes every 10 seconds
        int keepcnt = 3;        // Close after 3 failed probes
        setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        if (connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            logger_.error("connect() failed: " + std::string(strerror(errno)) +
                         " (errno=" + std::to_string(errno) + ")");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        logger_.debug("Socket connected successfully (fd=" + std::to_string(fd_) + ")");
        return true;
    }

    bool send_command_locked(const std::string& cmd) {
        if (fd_ < 0) {
            logger_.error("Cannot send command, socket not connected");
            return false;
        }

        std::string full_cmd = cmd + "\r";
        ssize_t sent = send(fd_, full_cmd.c_str(), full_cmd.length(), MSG_NOSIGNAL);
        bool success = sent == static_cast<ssize_t>(full_cmd.length());

        if (!success) {
            logger_.error("Failed to send command: " + cmd);
        } else {
            logger_.debug("Sent command: " + cmd);
        }

        return success;
    }

    std::string read_response(int timeout_ms = 500, const std::string& expected_prefix = "") const {
        if (fd_ < 0) return "";

        std::string accumulated;
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();

            if (elapsed >= timeout_ms) {
                return accumulated;
            }

            int remaining_ms = timeout_ms - elapsed;
            fd_set readfds;
            struct timeval tv;
            tv.tv_sec = remaining_ms / 1000;
            tv.tv_usec = (remaining_ms % 1000) * 1000;

            FD_ZERO(&readfds);
            FD_SET(fd_, &readfds);

            int ret = select(fd_ + 1, &readfds, nullptr, nullptr, &tv);
            if (ret <= 0) {
                return accumulated;
            }

            char buffer[256];
            ssize_t n = recv(fd_, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) {
                return accumulated;
            }

            buffer[n] = '\0';
            accumulated += buffer;

            if (!expected_prefix.empty() && accumulated.find(expected_prefix) != std::string::npos) {
                return accumulated;
            }
        }
    }

    int parse_volume_response(const std::string& response) {
        size_t pos = response.find("MV");
        if (pos == std::string::npos) return -1;

        // Check for MVMAX in response and extract max volume
        size_t mvmax_pos = response.find("MVMAX ");
        if (mvmax_pos != std::string::npos) {
            std::string max_str = response.substr(mvmax_pos + 6);
            size_t max_end = 0;
            while (max_end < max_str.length() && isdigit(max_str[max_end])) {
                max_end++;
            }
            if (max_end > 0) {
                try {
                    int detected_max = std::stoi(max_str.substr(0, max_end));
                    if (detected_max >= 1 && detected_max <= DENON_MAX_VOLUME) {
                        if (detected_max != max_volume_) {
                            max_volume_ = detected_max;
                            logger_.info("Detected receiver max volume: " + std::to_string(max_volume_));
                        }
                    }
                } catch (...) {
                    // Ignore parsing errors
                }
            }
        }

        std::string vol_str = response.substr(pos + 2);
        size_t end = 0;
        while (end < vol_str.length() && isdigit(vol_str[end])) {
            end++;
        }
        if (end == 0 || end > 3) return -1;

        vol_str = vol_str.substr(0, end);
        try {
            int vol_int = std::stoi(vol_str);
            float denon_vol;
            if (vol_str.length() == 2) {
                denon_vol = vol_int;
            } else {
                denon_vol = vol_int / 10.0f;
            }
            return static_cast<int>((denon_vol * 100.0f) / max_volume_ + 0.5f);
        } catch (...) {
            return -1;
        }
    }

    void probe_initial_state() {
        logger_.info("Probing initial receiver state");

        // Query volume (MV?) - response includes both MV and MVMAX
        // Denon receivers return: "MV70\rMVMAX 70\r"
        if (send_command_locked("MV?")) {
            std::string response = read_response(1000, "MVMAX");  // Wait for MVMAX to get complete response
            logger_.debug("MV response: " + response);

            // parse_volume_response() will detect MVMAX in the response and update max_volume_
            int vol = parse_volume_response(response);
            if (vol >= 0) {
                volume_ = vol;
                logger_.info("Probed initial volume: " + std::to_string(volume_) +
                           " (max=" + std::to_string(max_volume_) + ")");
            } else {
                logger_.warn("Failed to probe volume, using default: 50");
            }
        }

        // Query mute state (MU?)
        if (send_command_locked("MU?")) {
            std::string response = read_response(500, "MU");
            if (response.find("MUON") != std::string::npos) {
                mute_ = true;
                logger_.info("Probed initial mute: ON");
            } else if (response.find("MUOFF") != std::string::npos) {
                mute_ = false;
                logger_.info("Probed initial mute: OFF");
            } else {
                mute_ = false;
                logger_.debug("Failed to probe mute, defaulting to OFF");
            }
        }

        // Query power state (PW?)
        if (send_command_locked("PW?")) {
            std::string response = read_response(500, "PW");
            if (response.find("PWON") != std::string::npos) {
                power_ = true;
                logger_.info("Probed initial power: ON");
            } else if (response.find("PWSTANDBY") != std::string::npos) {
                power_ = false;
                logger_.info("Probed initial power: STANDBY");
            } else {
                power_ = true;  // Default to on if probe fails
                logger_.debug("Failed to probe power, defaulting to ON");
            }
        }

        // Query current input (SI?)
        if (send_command_locked("SI?")) {
            std::string response = read_response(500, "SI");
            size_t si_pos = response.find("SI");
            if (si_pos != std::string::npos) {
                std::string input_code = response.substr(si_pos + 2);
                // Extract input name (until CR or whitespace)
                size_t end = input_code.find_first_of("\r\n ");
                if (end != std::string::npos) {
                    input_code = input_code.substr(0, end);
                }
                if (!input_code.empty()) {
                    current_input_ = input_code;
                    original_input_ = input_code;  // Save for restoration on close
                    logger_.info("Probed initial input: " + current_input_);
                }
            } else {
                logger_.debug("Failed to probe current input");
            }
        }

        // Note: We don't probe speaker levels on init since they're rarely used
        // and slow to query. They'll be populated on demand if needed.
    }

    std::string get_speaker_code(Speaker speaker) const {
        switch (speaker) {
            case Speaker::FRONT_LEFT: return "FL";
            case Speaker::FRONT_RIGHT: return "FR";
            case Speaker::CENTER: return "C";
            case Speaker::SUBWOOFER: return "SW";
            case Speaker::SURROUND_LEFT: return "SL";
            case Speaker::SURROUND_RIGHT: return "SR";
            case Speaker::SURROUND_BACK_LEFT: return "SBL";
            case Speaker::SURROUND_BACK_RIGHT: return "SBR";
            case Speaker::SURROUND_BACK: return "SB";
            case Speaker::FRONT_HEIGHT_LEFT: return "FHL";
            case Speaker::FRONT_HEIGHT_RIGHT: return "FHR";
            default: return "";
        }
    }

public:
    DenonConnection(const std::string& host, int port,
                    const std::string& log_file, const std::string& log_level)
        : host_(host), port_(port) {

        logger_.init(log_file, log_level);
        logger_.info("Creating new Denon connection to " + host + ":" + std::to_string(port));

        // Connect socket
        connected_ = connect_socket();

        if (connected_) {
            logger_.info("Connected successfully");
            // Probe receiver state
            probe_initial_state();
        } else {
            logger_.error("Failed to connect");
        }

        last_used_ = time(nullptr);
    }

    ~DenonConnection() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (fd_ >= 0) {
            // Restore original input if it changed
            if (!original_input_.empty() && original_input_ != current_input_) {
                logger_.info("Restoring original input: " + original_input_ +
                           " (was: " + current_input_ + ")");
                std::string cmd = "SI" + original_input_;
                send_command_locked(cmd);
            }

            logger_.info("Closing Denon connection");
            ::close(fd_);
            fd_ = -1;
        }
    }

    // State accessors (thread-safe)
    int get_volume() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return volume_;
    }

    bool set_volume(int vol) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (vol < 0 || vol > 100) {
            logger_.warn("Invalid volume level: " + std::to_string(vol));
            return false;
        }

        int denon_vol = (vol * max_volume_) / 100;
        std::ostringstream cmd;
        cmd << "MV" << std::setw(2) << std::setfill('0') << denon_vol;

        logger_.debug("Setting volume to " + std::to_string(vol) + " (Denon: " + std::to_string(denon_vol) + ")");

        if (send_command_locked(cmd.str())) {
            volume_ = vol;
            return true;
        }
        logger_.error("Failed to set volume");
        return false;
    }

    bool volume_up() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (send_command_locked("MVUP")) {
            if (volume_ < 100) volume_++;
            return true;
        }
        return false;
    }

    bool volume_down() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (send_command_locked("MVDOWN")) {
            if (volume_ > 0) volume_--;
            return true;
        }
        return false;
    }

    bool get_mute() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return mute_;
    }

    bool get_power() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return power_;
    }

    std::string get_current_input() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return current_input_;
    }

    bool set_mute(bool muted) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        logger_.info(muted ? "Muting" : "Unmuting");
        bool result = send_command_locked(muted ? "MUON" : "MUOFF");
        if (result) {
            mute_ = muted;
        } else {
            logger_.error("Failed to set mute state");
        }
        return result;
    }

    bool set_power(bool on) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        logger_.info(on ? "Powering on" : "Powering off (standby)");
        bool result = send_command_locked(on ? "PWON" : "PWSTANDBY");
        if (result) {
            power_ = on;
        } else {
            logger_.error("Failed to set power state");
        }
        return result;
    }

    bool should_switch_input(const std::string& desired_input, int cooldown_seconds = 10) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (desired_input.empty()) return false;

        // Already on correct input
        if (current_input_ == desired_input) {
            return false;
        }

        // Different input - check cooldown
        time_t now = time(nullptr);
        if (now - last_input_switch_ >= cooldown_seconds) {
            return true;
        }

        return false;
    }

    bool set_input(const std::string& input) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        logger_.info("Setting input to: " + input);
        if (send_command_locked("SI" + input)) {
            current_input_ = input;
            last_input_switch_ = time(nullptr);
            return true;
        }
        return false;
    }

    std::vector<std::string> get_available_inputs() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::vector<std::string> inputs;

        // Return default inputs (we don't probe SSFUN anymore due to speed)
        return {
            "TUNER", "DVD", "BD", "TV", "SAT/CBL",
            "MPLAY", "GAME", "NET", "AUX1", "AUX2", "BT"
        };
    }

    bool set_speaker_level(Speaker speaker, int level) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (level < 38 || level > 62) {
            logger_.warn("Invalid speaker level: " + std::to_string(level));
            return false;
        }

        std::string speaker_code = get_speaker_code(speaker);
        if (speaker_code.empty()) {
            logger_.error("Unknown speaker type");
            return false;
        }

        logger_.info("Setting " + speaker_code + " level to: " + std::to_string(level));
        if (send_command_locked("SSLEV" + speaker_code + " " + std::to_string(level))) {
            speaker_levels_[speaker] = level;
            usleep(50000);
            return true;
        }
        return false;
    }

    int get_speaker_level(Speaker speaker) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = speaker_levels_.find(speaker);
        if (it != speaker_levels_.end()) {
            return it->second;
        }
        return 50;  // Default: 0dB reference
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return connected_ && fd_ >= 0;
    }

    int get_max_volume() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return max_volume_;
    }

    void mark_used() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_used_ = time(nullptr);
    }
};

// Connection pool for shared connections
class DenonConnectionPool {
private:
    static std::map<std::string, std::shared_ptr<DenonConnection>> connections_;
    static std::mutex mutex_;

    static std::string make_key(const std::string& host, int port) {
        return host + ":" + std::to_string(port);
    }

public:
    static std::shared_ptr<DenonConnection> acquire(
        const std::string& host, int port,
        const std::string& log_file, const std::string& log_level) {

        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = make_key(host, port);

        auto it = connections_.find(key);
        if (it != connections_.end()) {
            // Reuse existing connection
            it->second->mark_used();
            return it->second;
        }

        // Create new connection (does all probing)
        auto conn = std::make_shared<DenonConnection>(host, port, log_file, log_level);
        if (conn->is_connected()) {
            connections_[key] = conn;
            return conn;
        }

        return nullptr;
    }
};

// Static initialization
std::map<std::string, std::shared_ptr<DenonConnection>> DenonConnectionPool::connections_;
std::mutex DenonConnectionPool::mutex_;

// Backend implementation - thin wrapper around shared connection
class DenonIPBackend : public Backend {
private:
    std::shared_ptr<DenonConnection> conn_;
    std::string input_to_switch_;  // Per-instance config for auto-switching

public:
    bool init(const std::map<std::string, std::string>& config) override {
        auto host_it = config.find("host");
        if (host_it == config.end()) {
            return false;
        }
        std::string host = host_it->second;

        int port = 23;
        auto port_it = config.find("port");
        if (port_it != config.end()) {
            try {
                port = std::stoi(port_it->second);
            } catch (...) {
                port = 23;
            }
        }

        std::string log_file = config.count("log_file") ? config.at("log_file") : "";
        std::string log_level = config.count("log_level") ? config.at("log_level") : "INFO";

        // Get optional input switching config
        auto input_it = config.find("input");
        if (input_it != config.end()) {
            input_to_switch_ = input_it->second;
        }

        // Acquire shared connection (creates and probes if new)
        conn_ = DenonConnectionPool::acquire(host, port, log_file, log_level);
        if (!conn_) return false;

        // Auto-switch input if configured (only on initial connection)
        // Check if input needs switching (not already on desired input)
        if (!input_to_switch_.empty()) {
            std::string current = conn_->get_current_input();
            if (current != input_to_switch_) {
                conn_->set_input(input_to_switch_);
            }
        }

        return true;
    }

    void close() override {
        // Connection stays alive (shared)
        conn_.reset();
    }

    BackendCaps capabilities() const override {
        return {
            .supports_absolute = true,
            .supports_query = false,
            .supports_mute = true,
            .supports_input_select = true,
            .supports_speaker_level = true,
            .min_volume = 0,
            .max_volume = 100
        };
    }

    bool set_volume_absolute(int level) override {
        if (!conn_) return false;
        return conn_->set_volume(level);
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
        if (!conn_) return false;
        return conn_->set_mute(muted);
    }

    std::optional<bool> get_mute() override {
        return conn_ ? std::optional<bool>(conn_->get_mute()) : std::nullopt;
    }

    bool set_power(bool on) override {
        return conn_ ? conn_->set_power(on) : false;
    }

    std::optional<bool> get_power() override {
        return conn_ ? std::optional<bool>(conn_->is_connected()) : std::nullopt;
    }

    std::string name() const override {
        return "Denon IP";
    }

    bool is_connected() const override {
        return conn_ && conn_->is_connected();
    }

    int get_hardware_max_volume() const override {
        return conn_ ? conn_->get_max_volume() : 98;
    }

    std::vector<std::string> get_available_inputs() override {
        return conn_ ? conn_->get_available_inputs() : std::vector<std::string>{};
    }

    bool set_input(const std::string& input) override {
        if (!conn_) return false;
        bool result = conn_->set_input(input);
        if (result) {
            usleep(100000);  // Wait 100ms for input switch
        }
        return result;
    }

    std::optional<std::string> get_current_input() override {
        if (!conn_) return std::nullopt;
        std::string input = conn_->get_current_input();
        return input.empty() ? std::nullopt : std::optional<std::string>(input);
    }

    std::vector<Speaker> get_available_speakers() override {
        return {
            Speaker::FRONT_LEFT, Speaker::FRONT_RIGHT,
            Speaker::CENTER, Speaker::SUBWOOFER,
            Speaker::SURROUND_LEFT, Speaker::SURROUND_RIGHT,
            Speaker::SURROUND_BACK_LEFT, Speaker::SURROUND_BACK_RIGHT,
            Speaker::SURROUND_BACK, Speaker::FRONT_HEIGHT_LEFT, Speaker::FRONT_HEIGHT_RIGHT
        };
    }

    bool set_speaker_level(Speaker speaker, int level) override {
        return conn_ ? conn_->set_speaker_level(speaker, level) : false;
    }

    int get_speaker_level(Speaker speaker) override {
        return conn_ ? conn_->get_speaker_level(speaker) : 50;
    }

    bool reconnect() override {
        // For shared connections, we don't support explicit reconnect
        // The connection is managed by the pool
        return conn_ && conn_->is_connected();
    }
};

// Factory function
std::unique_ptr<Backend> create_denon_ip_backend() {
    return std::make_unique<DenonIPBackend>();
}

} // namespace AlsaAVR

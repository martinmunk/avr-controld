#include "../include/shared_state.hpp"
#include <map>
#include <string>
#include <ctime>
#include <mutex>

namespace AlsaAVR {

// Simple stub implementation for test_backend
// The real implementation is in alsa_plugin.cpp

struct InputSwitchInfo {
    std::string last_input;
    time_t last_switch_time;
};

static std::map<std::string, InputSwitchInfo> input_switch_state;
static std::mutex state_mutex;

bool SharedState::should_switch_input(const std::string& host, int port,
                                      const std::string& desired_input, int cooldown_seconds) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::string key = host + ":" + std::to_string(port);

    auto it = input_switch_state.find(key);
    if (it == input_switch_state.end()) {
        // No previous switch recorded
        return true;
    }

    // Check if cooldown period has elapsed
    time_t now = time(nullptr);
    if (now - it->second.last_switch_time >= cooldown_seconds) {
        return true;
    }

    // If we're already on the desired input, no need to switch
    if (it->second.last_input == desired_input) {
        return false;
    }

    return false;  // Within cooldown, don't switch
}

void SharedState::record_input_switch(const std::string& host, int port, const std::string& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::string key = host + ":" + std::to_string(port);

    InputSwitchInfo info;
    info.last_input = input;
    info.last_switch_time = time(nullptr);

    input_switch_state[key] = info;
}

// Serial backend methods
bool SharedState::should_switch_input_serial(const std::string& device_path,
                                            const std::string& desired_input, int cooldown_seconds) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::string key = "serial:" + device_path;

    auto it = input_switch_state.find(key);
    if (it == input_switch_state.end()) {
        // No previous switch recorded
        return true;
    }

    // Check if cooldown period has elapsed
    time_t now = time(nullptr);
    if (now - it->second.last_switch_time >= cooldown_seconds) {
        return true;
    }

    // If we're already on the desired input, no need to switch
    if (it->second.last_input == desired_input) {
        return false;
    }

    return false;  // Within cooldown, don't switch
}

void SharedState::record_input_switch_serial(const std::string& device_path, const std::string& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    std::string key = "serial:" + device_path;

    InputSwitchInfo info;
    info.last_input = input;
    info.last_switch_time = time(nullptr);

    input_switch_state[key] = info;
}

} // namespace AlsaAVR

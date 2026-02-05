#pragma once

#include <string>

namespace AlsaAVR {

// Forward declaration for connection pool access
class SharedState {
public:
    // TCP/IP backends (Denon)
    static bool should_switch_input(const std::string& host, int port,
                                    const std::string& desired_input, int cooldown_seconds = 10);
    static void record_input_switch(const std::string& host, int port, const std::string& input);

    // Serial backends (Opalum, NAD)
    static bool should_switch_input_serial(const std::string& device_path,
                                          const std::string& desired_input, int cooldown_seconds = 10);
    static void record_input_switch_serial(const std::string& device_path, const std::string& input);

    // Volume/mute state caching
    static void set_volume(const std::string& host, int port, int volume);
    static int get_volume(const std::string& host, int port, int default_value = 50);
    static void set_mute(const std::string& host, int port, bool muted);
    static bool get_mute(const std::string& host, int port, bool default_value = false);
    static bool has_cached_volume(const std::string& host, int port);
};

} // namespace AlsaAVR

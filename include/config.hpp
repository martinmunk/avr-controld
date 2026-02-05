#pragma once

#include <string>
#include <map>

namespace AlsaAVR {

struct Config {
    std::string backend_type;  // "denon_ip", "cec", "opalum_serial"
    std::map<std::string, std::string> backend_config;

    // ALSA control settings
    std::string control_name = "AVR Volume";
    int poll_interval_ms = 1000;  // How often to query volume
    bool sync_on_startup = true;

    // Load from file
    static Config load(const std::string& path);

    // Parse from string (for testing)
    static Config parse(const std::string& content);
};

} // namespace AlsaAVR

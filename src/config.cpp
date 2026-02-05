#include "../include/config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace AlsaAVR {

static std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

Config Config::parse(const std::string& content) {
    Config config;
    std::istringstream stream(content);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Section header
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            current_section = line.substr(1, line.length() - 2);
            current_section = trim(current_section);
            continue;
        }

        // Key-value pair
        size_t equals_pos = line.find('=');
        if (equals_pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, equals_pos));
        std::string value = trim(line.substr(equals_pos + 1));

        // Remove quotes from value if present
        if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"') {
            value = value.substr(1, value.length() - 2);
        }

        if (current_section.empty()) {
            // Global configuration
            if (key == "backend") {
                config.backend_type = value;
            } else if (key == "control_name") {
                config.control_name = value;
            } else if (key == "poll_interval_ms") {
                config.poll_interval_ms = std::stoi(value);
            } else if (key == "sync_on_startup") {
                config.sync_on_startup = (value == "true" || value == "1");
            }
        } else if (current_section == "backend") {
            // Backend-specific configuration
            config.backend_config[key] = value;
        }
    }

    return config;
}

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Config();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

} // namespace AlsaAVR

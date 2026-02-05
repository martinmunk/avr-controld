#include "include/backend.hpp"
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <chrono>

namespace AlsaAVR {
extern std::unique_ptr<Backend> create_denon_ip_backend();
}

int main() {
    std::cout << "\n=== Testing Input Switching on Init ===\n\n";

    std::map<std::string, std::string> config;
    config["host"] = "192.168.20.216";
    config["port"] = "23";
    config["input"] = "MPLAY";  // Should switch to this on init
    config["log_file"] = "/tmp/input-switch-test.log";
    config["log_level"] = "DEBUG";

    std::cout << "Creating backend with input=MPLAY...\n";
    auto backend = AlsaAVR::create_denon_ip_backend();
    
    if (!backend->init(config)) {
        std::cerr << "Failed to initialize backend\n";
        return 1;
    }

    std::cout << "Backend initialized successfully\n";
    std::cout << "Waiting 2 seconds for input switch to complete...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::string current_input = backend->get_current_input();
    std::cout << "Current input: " << current_input << "\n";

    backend->close();

    std::cout << "\n=== Check /tmp/input-switch-test.log ===\n";
    std::cout << "You should see 'Setting input to: MPLAY' in the log\n\n";

    if (current_input == "MPLAY") {
        std::cout << "✓ SUCCESS: Input was switched to MPLAY\n";
        return 0;
    } else {
        std::cout << "✗ FAILED: Input is " << current_input << ", expected MPLAY\n";
        return 1;
    }
}

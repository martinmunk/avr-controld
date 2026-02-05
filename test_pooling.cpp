#include "include/backend.hpp"
#include <iostream>
#include <memory>
#include <map>
#include <string>

namespace AlsaAVR {
extern std::unique_ptr<Backend> create_denon_ip_backend();
}

int main() {
    std::cout << "\n=== Testing Connection Pooling Within Single Process ===\n\n";

    std::map<std::string, std::string> config;
    config["host"] = "192.168.20.216";
    config["port"] = "23";
    config["log_file"] = "/tmp/pool-test.log";
    config["log_level"] = "DEBUG";

    // Create first backend instance
    std::cout << "Creating first backend instance...\n";
    auto backend1 = AlsaAVR::create_denon_ip_backend();
    if (!backend1->init(config)) {
        std::cerr << "Failed to initialize first backend\n";
        return 1;
    }
    int vol1 = backend1->get_volume();
    std::cout << "First backend: volume = " << vol1 << "\n";
    backend1->close();
    std::cout << "First backend closed\n\n";

    // Create second backend instance (should reuse connection)
    std::cout << "Creating second backend instance (should reuse connection)...\n";
    auto backend2 = AlsaAVR::create_denon_ip_backend();
    if (!backend2->init(config)) {
        std::cerr << "Failed to initialize second backend\n";
        return 1;
    }
    int vol2 = backend2->get_volume();
    std::cout << "Second backend: volume = " << vol2 << "\n";
    backend2->close();
    std::cout << "Second backend closed\n\n";

    // Create third backend instance (should also reuse connection)
    std::cout << "Creating third backend instance (should also reuse connection)...\n";
    auto backend3 = AlsaAVR::create_denon_ip_backend();
    if (!backend3->init(config)) {
        std::cerr << "Failed to initialize third backend\n";
        return 1;
    }
    int vol3 = backend3->get_volume();
    std::cout << "Third backend: volume = " << vol3 << "\n";
    backend3->close();
    std::cout << "Third backend closed\n\n";

    std::cout << "=== Check /tmp/pool-test.log ===\n";
    std::cout << "You should see:\n";
    std::cout << "  - 'Creating new Denon connection' only ONCE\n";
    std::cout << "  - 'Probing initial receiver state' only ONCE\n";
    std::cout << "  - Subsequent backend inits should be fast with no MV? queries\n";

    return 0;
}

#include "../include/backend.hpp"
#include <map>
#include <functional>
#include <memory>

namespace AlsaAVR {

// Forward declarations of backend creation functions
// These are defined in their respective backend implementation files
extern std::unique_ptr<Backend> create_denon_ip();
extern std::unique_ptr<Backend> create_cec();
extern std::unique_ptr<Backend> create_opalum_serial();
extern std::unique_ptr<Backend> create_nad_serial_backend();

// Backend registry
static std::map<std::string, std::function<std::unique_ptr<Backend>()>> backend_registry;

// Initialize registry
static void init_registry() {
    static bool initialized = false;
    if (initialized) return;

    backend_registry["denon_ip"] = []() -> std::unique_ptr<Backend> {
        // DenonIPBackend is defined in denon_ip.cpp
        // We need to expose it via a factory function
        extern std::unique_ptr<Backend> create_denon_ip_backend();
        return create_denon_ip_backend();
    };

    backend_registry["cec"] = []() -> std::unique_ptr<Backend> {
        extern std::unique_ptr<Backend> create_cec_backend();
        return create_cec_backend();
    };

    backend_registry["opalum_serial"] = []() -> std::unique_ptr<Backend> {
        extern std::unique_ptr<Backend> create_opalum_serial_backend();
        return create_opalum_serial_backend();
    };

    backend_registry["nad_serial"] = []() -> std::unique_ptr<Backend> {
        extern std::unique_ptr<Backend> create_nad_serial_backend();
        return create_nad_serial_backend();
    };

    // Backwards compatibility aliases
    backend_registry["opalum_rs232"] = []() -> std::unique_ptr<Backend> {
        extern std::unique_ptr<Backend> create_opalum_serial_backend();
        return create_opalum_serial_backend();
    };

    backend_registry["nad_uart"] = []() -> std::unique_ptr<Backend> {
        extern std::unique_ptr<Backend> create_nad_serial_backend();
        return create_nad_serial_backend();
    };

    initialized = true;
}

std::unique_ptr<Backend> BackendFactory::create(const std::string& type) {
    init_registry();

    auto it = backend_registry.find(type);
    if (it == backend_registry.end()) {
        return nullptr;
    }

    return it->second();
}

std::vector<std::string> BackendFactory::available_backends() {
    init_registry();

    std::vector<std::string> backends;
    for (const auto& pair : backend_registry) {
        backends.push_back(pair.first);
    }
    return backends;
}

} // namespace AlsaAVR

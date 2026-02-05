#include "../include/backend.hpp"
#include "../include/logger.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>

using namespace AlsaAVR;

// Forward declare factory functions
namespace AlsaAVR {
    extern std::unique_ptr<Backend> create_denon_ip_backend();
}

// ANSI color codes for output
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"

// Test result tracking
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

class HardwareTestSuite {
private:
    std::unique_ptr<Backend> backend_;
    std::vector<TestResult> results_;
    int test_number_ = 0;

    void print_header(const std::string& title) {
        std::cout << "\n" << COLOR_CYAN << std::string(70, '=') << COLOR_RESET << std::endl;
        std::cout << COLOR_CYAN << "  " << title << COLOR_RESET << std::endl;
        std::cout << COLOR_CYAN << std::string(70, '=') << COLOR_RESET << std::endl;
    }

    void print_test_start(const std::string& test_name) {
        test_number_++;
        std::cout << "\n" << COLOR_BLUE << "[" << test_number_ << "] "
                  << test_name << "..." << COLOR_RESET << std::endl;
    }

    void record_pass(const std::string& test_name, const std::string& message = "") {
        results_.push_back({test_name, true, message});
        std::cout << COLOR_GREEN << "  ✓ PASS" << COLOR_RESET;
        if (!message.empty()) {
            std::cout << ": " << message;
        }
        std::cout << std::endl;
    }

    void record_fail(const std::string& test_name, const std::string& message) {
        results_.push_back({test_name, false, message});
        std::cout << COLOR_RED << "  ✗ FAIL" << COLOR_RESET;
        if (!message.empty()) {
            std::cout << ": " << message;
        }
        std::cout << std::endl;
    }

    void wait_for_command(int ms = 300) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

public:
    HardwareTestSuite(std::unique_ptr<Backend> backend) : backend_(std::move(backend)) {}

    // Test 1: Basic connectivity
    void test_connectivity() {
        print_test_start("Basic Connectivity Test");
        if (!backend_->is_connected()) {
            record_fail("Connectivity", "Backend reports not connected");
            return;
        }
        record_pass("Connectivity", "Backend connected successfully");
    }

    // Test 2: Volume query
    void test_volume_query() {
        print_test_start("Volume Query Test");
        int vol = backend_->get_volume();
        if (vol < 0 || vol > 100) {
            record_fail("Volume Query", "Invalid volume: " + std::to_string(vol));
            return;
        }
        record_pass("Volume Query", "Current volume: " + std::to_string(vol) + "%");
    }

    // Test 3: Mute query
    void test_mute_query() {
        print_test_start("Mute Query Test");
        bool muted = backend_->get_mute();
        record_pass("Mute Query", std::string("Current mute state: ") + (muted ? "ON" : "OFF"));
    }

    // Test 4: Power query
    void test_power_query() {
        print_test_start("Power Query Test");
        bool power = backend_->get_power();
        record_pass("Power Query", std::string("Current power state: ") + (power ? "ON" : "OFF"));
    }

    // Test 5: Volume set and verify
    void test_volume_set_verify() {
        print_test_start("Volume Set & Verify Test");
        int original_vol = backend_->get_volume();
        wait_for_command();

        if (!backend_->set_volume_absolute(50)) {
            record_fail("Volume Set & Verify", "Failed to set volume to 50%");
            return;
        }
        wait_for_command(500);

        int read_vol = backend_->get_volume();
        if (read_vol < 45 || read_vol > 55) {
            record_fail("Volume Set & Verify",
                       "Set 50%, but read " + std::to_string(read_vol) + "%");
            backend_->set_volume_absolute(original_vol);
            return;
        }

        backend_->set_volume_absolute(original_vol);
        wait_for_command();

        record_pass("Volume Set & Verify",
                   "Set 50%, verified " + std::to_string(read_vol) + "%");
    }

    // Test 6: Mute toggle
    void test_mute_toggle() {
        print_test_start("Mute Toggle Test");
        bool original_mute = backend_->get_mute();
        wait_for_command();

        // Toggle mute
        if (!backend_->set_mute(!original_mute)) {
            record_fail("Mute Toggle", "Failed to toggle mute");
            return;
        }
        wait_for_command(500);

        bool new_mute = backend_->get_mute();
        if (new_mute == original_mute) {
            record_fail("Mute Toggle", "Mute state did not change");
            backend_->set_mute(original_mute);
            return;
        }

        // Restore original state
        backend_->set_mute(original_mute);
        wait_for_command();

        record_pass("Mute Toggle", "Successfully toggled mute");
    }

    void run_all_tests() {
        print_header("HARDWARE TEST SUITE - Denon AVR");

        test_connectivity();
        test_volume_query();
        test_mute_query();
        test_power_query();
        test_volume_set_verify();
        test_mute_toggle();

        print_summary();
    }

    void print_summary() {
        int passed = 0;
        int failed = 0;

        for (const auto& result : results_) {
            if (result.passed) passed++;
            else failed++;
        }

        std::cout << "\n" << COLOR_CYAN << std::string(70, '=') << COLOR_RESET << std::endl;
        std::cout << COLOR_CYAN << "  TEST SUMMARY" << COLOR_RESET << std::endl;
        std::cout << COLOR_CYAN << std::string(70, '=') << COLOR_RESET << std::endl;

        std::cout << COLOR_GREEN << "  Passed: " << passed << COLOR_RESET << std::endl;
        if (failed > 0) {
            std::cout << COLOR_RED << "  Failed: " << failed << COLOR_RESET << std::endl;
        } else {
            std::cout << "  Failed: " << failed << std::endl;
        }

        int total = passed + failed;
        int percentage = total > 0 ? (passed * 100) / total : 0;

        std::cout << "  Total:  " << total << std::endl;

        if (percentage >= 80) {
            std::cout << COLOR_GREEN;
        } else if (percentage >= 50) {
            std::cout << COLOR_YELLOW;
        } else {
            std::cout << COLOR_RED;
        }
        std::cout << "  Success Rate: " << percentage << "%" << COLOR_RESET << std::endl;

        if (failed > 0) {
            std::cout << "\n" << COLOR_RED << "  Failed Tests:" << COLOR_RESET << std::endl;
            for (const auto& result : results_) {
                if (!result.passed) {
                    std::cout << "    - " << result.name;
                    if (!result.message.empty()) {
                        std::cout << ": " << result.message;
                    }
                    std::cout << std::endl;
                }
            }
        }

        std::cout << COLOR_CYAN << std::string(70, '=') << COLOR_RESET << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <backend_type> <host> <port>" << std::endl;
        std::cerr << "Example: " << argv[0] << " denon_ip 192.168.20.107 23" << std::endl;
        return 1;
    }

    std::string backend_type = argv[1];
    std::string host = argv[2];
    std::string port = argv[3];

    // Create backend
    std::map<std::string, std::string> config;
    config["host"] = host;
    config["port"] = port;
    config["log_level"] = "DEBUG";

    std::unique_ptr<Backend> backend;
    if (backend_type == "denon_ip") {
        backend = create_denon_ip_backend();
    } else {
        std::cerr << "Unknown backend type: " << backend_type << std::endl;
        return 1;
    }

    if (!backend->init(config)) {
        std::cerr << "Failed to initialize backend" << std::endl;
        return 1;
    }

    // Run tests
    HardwareTestSuite suite(std::move(backend));
    suite.run_all_tests();

    return 0;
}

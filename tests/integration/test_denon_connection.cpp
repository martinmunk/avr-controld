#include <gtest/gtest.h>
#include "../../include/backend.hpp"
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace AlsaAVR;

// Forward declare factory function (it's in AlsaAVR namespace)
namespace AlsaAVR {
    extern std::unique_ptr<Backend> create_denon_ip_backend();
}

// Test fixture for real hardware integration tests
class DenonConnectionTest : public ::testing::Test {
protected:
    std::unique_ptr<Backend> backend_;
    std::string test_host_;
    std::string test_port_;
    bool hardware_available_ = false;

    void SetUp() override {
        // Check if hardware test configuration is provided via environment
        const char* host_env = std::getenv("DENON_TEST_HOST");
        const char* port_env = std::getenv("DENON_TEST_PORT");

        if (host_env) {
            test_host_ = host_env;
            test_port_ = port_env ? port_env : "23";
            hardware_available_ = true;
        } else {
            // Skip hardware tests if no config provided
            hardware_available_ = false;
        }
    }

    void TearDown() override {
        if (backend_) {
            backend_->close();
        }
    }

    // Helper to initialize backend
    bool init_backend(const std::map<std::string, std::string>& extra_config = {}) {
        backend_ = create_denon_ip_backend();
        if (!backend_) return false;

        std::map<std::string, std::string> config = {
            {"host", test_host_},
            {"port", test_port_},
            {"log_level", "DEBUG"}
        };

        // Merge in extra config
        for (const auto& [key, value] : extra_config) {
            config[key] = value;
        }

        return backend_->init(config);
    }

    // Macro to skip test if hardware not available
    #define SKIP_IF_NO_HARDWARE() \
        if (!hardware_available_) { \
            GTEST_SKIP() << "Hardware tests skipped (set DENON_TEST_HOST to enable)"; \
        }

    // Helper to wait for command processing
    void wait_for_command(int ms = 300) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

// ============================================================================
// Basic Connectivity Tests
// ============================================================================

TEST_F(DenonConnectionTest, ConnectToReceiver) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    EXPECT_TRUE(backend_->is_connected());
}

TEST_F(DenonConnectionTest, CheckCapabilities) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    auto caps = backend_->capabilities();
    EXPECT_TRUE(caps.supports_absolute);
    EXPECT_TRUE(caps.supports_mute);
    EXPECT_TRUE(caps.supports_input_select);
    EXPECT_TRUE(caps.supports_speaker_level);
    EXPECT_EQ(caps.min_volume, 0);
    EXPECT_EQ(caps.max_volume, 100);
}

TEST_F(DenonConnectionTest, GetBackendName) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    EXPECT_EQ(backend_->name(), "Denon IP");
}

// ============================================================================
// Volume Query Tests
// ============================================================================

TEST_F(DenonConnectionTest, QueryVolume) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int vol = backend_->get_volume().value_or(-1);
    EXPECT_GE(vol, 0);
    EXPECT_LE(vol, 100);
}

TEST_F(DenonConnectionTest, QueryMaxVolume) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int max = backend_->get_hardware_max_volume();
    EXPECT_GE(max, 1);
    EXPECT_LE(max, 98);
}

// ============================================================================
// Volume Control Tests
// ============================================================================

TEST_F(DenonConnectionTest, SetVolumeAbsolute) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int original_vol = backend_->get_volume().value_or(-1);
    wait_for_command();

    // Set to 50%
    ASSERT_TRUE(backend_->set_volume_absolute(50));
    wait_for_command(500);

    int read_vol = backend_->get_volume().value_or(-1);
    EXPECT_GE(read_vol, 45);  // Allow some tolerance
    EXPECT_LE(read_vol, 55);

    // Restore original
    backend_->set_volume_absolute(original_vol);
    wait_for_command();
}

TEST_F(DenonConnectionTest, SetVolumeMinMax) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int original_vol = backend_->get_volume().value_or(-1);
    wait_for_command();

    // Test minimum (be careful with actual hardware!)
    ASSERT_TRUE(backend_->set_volume_absolute(10));
    wait_for_command(500);
    int vol_min = backend_->get_volume().value_or(-1);
    EXPECT_GE(vol_min, 8);
    EXPECT_LE(vol_min, 12);

    // Test mid-range
    ASSERT_TRUE(backend_->set_volume_absolute(40));
    wait_for_command(500);
    int vol_mid = backend_->get_volume().value_or(-1);
    EXPECT_GE(vol_mid, 38);
    EXPECT_LE(vol_mid, 42);

    // Restore original
    backend_->set_volume_absolute(original_vol);
    wait_for_command();
}

TEST_F(DenonConnectionTest, VolumeUpDown) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int original_vol = backend_->get_volume().value_or(-1);
    wait_for_command();

    // Volume up
    ASSERT_TRUE(backend_->volume_up());
    wait_for_command(300);
    int vol_after_up = backend_->get_volume().value_or(-1);
    EXPECT_GT(vol_after_up, original_vol);

    // Volume down
    ASSERT_TRUE(backend_->volume_down());
    wait_for_command(300);
    int vol_after_down = backend_->get_volume().value_or(-1);
    EXPECT_LT(vol_after_down, vol_after_up);

    // Restore original
    backend_->set_volume_absolute(original_vol);
    wait_for_command();
}

TEST_F(DenonConnectionTest, VolumeInvalidRange) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int original_vol = backend_->get_volume().value_or(-1);

    // Try to set out of range (should fail or clamp)
    backend_->set_volume_absolute(-10);
    wait_for_command(300);
    EXPECT_GE(backend_->get_volume(), 0);

    backend_->set_volume_absolute(150);
    wait_for_command(300);
    EXPECT_LE(backend_->get_volume(), 100);

    // Restore original
    backend_->set_volume_absolute(original_vol);
    wait_for_command();
}

// ============================================================================
// Mute Control Tests
// ============================================================================

TEST_F(DenonConnectionTest, QueryMute) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    bool muted = backend_->get_mute().value_or(false);
    // Just verify we can read it (could be either state)
    EXPECT_TRUE(muted == true || muted == false);
}

TEST_F(DenonConnectionTest, SetMuteOn) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    bool original_mute = backend_->get_mute().value_or(false);
    wait_for_command();

    // Mute
    ASSERT_TRUE(backend_->set_mute(true));
    wait_for_command(500);
    EXPECT_TRUE(backend_->get_mute().value_or(false));

    // Restore
    backend_->set_mute(original_mute);
    wait_for_command();
}

TEST_F(DenonConnectionTest, SetMuteOff) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    bool original_mute = backend_->get_mute().value_or(false);
    wait_for_command();

    // Unmute
    ASSERT_TRUE(backend_->set_mute(false));
    wait_for_command(500);
    EXPECT_FALSE(backend_->get_mute().value_or(true));

    // Restore
    backend_->set_mute(original_mute);
    wait_for_command();
}

TEST_F(DenonConnectionTest, MuteToggle) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    bool original_mute = backend_->get_mute().value_or(false);
    wait_for_command();

    // Toggle
    ASSERT_TRUE(backend_->set_mute(!original_mute));
    wait_for_command(500);
    EXPECT_EQ(backend_->get_mute(), !original_mute);

    // Toggle back
    ASSERT_TRUE(backend_->set_mute(original_mute));
    wait_for_command(500);
    EXPECT_EQ(backend_->get_mute(), original_mute);
}

// ============================================================================
// Power Control Tests
// ============================================================================

TEST_F(DenonConnectionTest, QueryPower) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    // If we're connected, power should be on
    bool power = backend_->get_power().value_or(false);
    EXPECT_TRUE(power);
}

// Note: We don't test power off as it would disrupt other tests
// and potentially confuse users. Power control is dangerous for
// automated tests on real hardware.

// ============================================================================
// Input Selection Tests
// ============================================================================

TEST_F(DenonConnectionTest, GetAvailableInputs) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    auto inputs = backend_->get_available_inputs();
    EXPECT_GT(inputs.size(), 0);

    // Check for common inputs
    bool has_common_input = false;
    for (const auto& input : inputs) {
        if (input == "MPLAY" || input == "DVD" || input == "TV" || input == "BD") {
            has_common_input = true;
            break;
        }
    }
    EXPECT_TRUE(has_common_input);
}

TEST_F(DenonConnectionTest, GetCurrentInput) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    std::string current = backend_->get_current_input().value_or("");
    EXPECT_FALSE(current.empty());
}

TEST_F(DenonConnectionTest, SetInput) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    std::string original_input = backend_->get_current_input().value_or("");
    wait_for_command();

    // Try switching to a common input (that likely exists)
    auto inputs = backend_->get_available_inputs();
    ASSERT_GT(inputs.size(), 0);

    // Find an input different from current
    std::string test_input;
    for (const auto& input : inputs) {
        if (input != original_input) {
            test_input = input;
            break;
        }
    }

    if (!test_input.empty()) {
        ASSERT_TRUE(backend_->set_input(test_input));
        wait_for_command(1000);  // Input switching takes time

        std::string new_input = backend_->get_current_input().value_or("");
        EXPECT_EQ(new_input, test_input);

        // Restore original
        backend_->set_input(original_input);
        wait_for_command(1000);
    }
}

// ============================================================================
// Input Auto-Switch Tests
// ============================================================================

// A raw second telnet session to the (mock) receiver, to poke it from outside
// the backend under test: change its state behind the backend's back, or make
// it drop the backend's session (XDROPOTHERS, a mock-only hook).
static bool raw_avr_command(const std::string& host, const std::string& port, const std::string& cmd) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(std::stoi(port));
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return false; }
    std::string line = cmd + "\r";
    bool ok = send(fd, line.data(), line.size(), MSG_NOSIGNAL) == (ssize_t)line.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::close(fd);
    return ok;
}

TEST_F(DenonConnectionTest, NoAutoSwitchOnInit) {
    SKIP_IF_NO_HARDWARE();

    // Put the receiver on DVD, then open a handle configured for MPLAY.
    ASSERT_TRUE(init_backend());
    std::string original_input = backend_->get_current_input().value_or("");
    backend_->set_input("DVD");
    wait_for_command(1000);
    EXPECT_EQ(backend_->get_current_input(), "DVD");
    backend_->close();
    backend_.reset();

    // The backend lives in a long-running daemon, so init must NOT touch the
    // input: switching is the session manager's job (avr-power reads
    // configured_input via "Input Source Default" and switches explicitly).
    std::map<std::string, std::string> config = {{"input", "MPLAY"}};
    ASSERT_TRUE(init_backend(config));
    wait_for_command(500);
    EXPECT_EQ(backend_->get_current_input(), "DVD");

    // An explicit switch still works, and volume changes never touch the input.
    ASSERT_TRUE(backend_->set_input("MPLAY"));
    wait_for_command(500);
    EXPECT_EQ(backend_->get_current_input(), "MPLAY");
    backend_->set_volume_absolute(50);
    wait_for_command(500);
    EXPECT_EQ(backend_->get_current_input(), "MPLAY");

    backend_->set_input(original_input);
    wait_for_command(1000);
}

// ============================================================================
// Long-lived connection: the receiver drops sessions and is driven by others
// ============================================================================

// The receiver closes idle telnet sessions (standby, network hiccups). A
// long-lived owner (avr-controld) must notice and reconnect, transparently to
// the caller - this is exactly what left StueTaerten with no volume/input
// control for days on 0.3.0.
TEST_F(DenonConnectionTest, ReconnectAfterReceiverDropsSession) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    ASSERT_TRUE(backend_->set_volume_absolute(30));

    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XDROPOTHERS"));
    wait_for_command(300);

    // Commands keep working (the first one re-establishes the session) and the
    // backend does not lie about being connected in between.
    EXPECT_TRUE(backend_->set_volume_absolute(37));
    EXPECT_TRUE(backend_->is_connected());
    auto v = backend_->get_volume();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(std::abs(*v - 37), 1);

    // ... and again, to be sure it is not a one-shot.
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XDROPOTHERS"));
    wait_for_command(300);
    auto in = backend_->get_current_input();
    ASSERT_TRUE(in.has_value());
    EXPECT_TRUE(backend_->is_connected());
    EXPECT_TRUE(backend_->set_input("MPLAY"));
    EXPECT_EQ(backend_->get_current_input(), "MPLAY");
}

// The receiver is also driven by its remote and by HDMI-CEC from the TV, so
// cached state in a long-lived owner goes stale. Reads must ask the receiver.
TEST_F(DenonConnectionTest, ReadsReflectExternalChanges) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    ASSERT_TRUE(backend_->set_power(true));
    ASSERT_TRUE(backend_->set_input("MPLAY"));
    ASSERT_TRUE(backend_->set_mute(false));
    EXPECT_EQ(backend_->get_power(), true);

    // Someone else (TV over CEC) puts it in standby and picks another input.
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "PWSTANDBY"));
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "SIBD"));
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "MUON"));
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "MV20"));

    EXPECT_EQ(backend_->get_power(), false);
    EXPECT_EQ(backend_->get_current_input(), "BD");
    EXPECT_EQ(backend_->get_mute(), true);
    auto v = backend_->get_volume();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(std::abs(*v - 20), 1);   // mock max-volume 98 -> ~1:1

    // Restore for the other tests.
    backend_->set_mute(false);
    backend_->set_power(true);
}

// ============================================================================
// Connection Pooling Tests
// ============================================================================

TEST_F(DenonConnectionTest, ConnectionReuse) {
    SKIP_IF_NO_HARDWARE();
    // Create first backend instance
    ASSERT_TRUE(init_backend());
    int vol1 = backend_->get_volume().value_or(-1);
    backend_->close();
    backend_.reset();

    wait_for_command(100);

    // Create second instance - should reuse connection
    ASSERT_TRUE(init_backend());
    int vol2 = backend_->get_volume().value_or(-1);

    // State should be preserved
    EXPECT_EQ(vol1, vol2);
}

TEST_F(DenonConnectionTest, MultipleClientsSimultaneous) {
    SKIP_IF_NO_HARDWARE();
    // Create two backend instances
    auto backend1 = create_denon_ip_backend();
    auto backend2 = create_denon_ip_backend();

    std::map<std::string, std::string> config = {
        {"host", test_host_},
        {"port", test_port_},
        {"log_level", "DEBUG"}
    };

    if (!hardware_available_) {
        GTEST_SKIP() << "Hardware tests skipped";
    }

    ASSERT_TRUE(backend1->init(config));
    ASSERT_TRUE(backend2->init(config));

    // Both should be connected
    EXPECT_TRUE(backend1->is_connected());
    EXPECT_TRUE(backend2->is_connected());

    // Volume changes from one should be visible to the other
    int original = backend1->get_volume().value_or(-1);
    backend1->set_volume_absolute(50);
    wait_for_command(500);

    EXPECT_EQ(backend2->get_volume(), 50);

    // Restore
    backend1->set_volume_absolute(original);
    wait_for_command();

    backend1->close();
    backend2->close();
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_F(DenonConnectionTest, RapidVolumeChanges) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());

    int original = backend_->get_volume().value_or(-1);

    // Rapidly change volume
    for (int i = 0; i < 10; i++) {
        backend_->volume_up();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    wait_for_command(500);

    for (int i = 0; i < 10; i++) {
        backend_->volume_down();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    wait_for_command(500);

    // Restore
    backend_->set_volume_absolute(original);
    wait_for_command();
}

TEST_F(DenonConnectionTest, RepeatedConnectDisconnect) {
    SKIP_IF_NO_HARDWARE();
    if (!hardware_available_) {
        GTEST_SKIP() << "Hardware tests skipped";
    }

    for (int i = 0; i < 5; i++) {
        auto backend = create_denon_ip_backend();
        std::map<std::string, std::string> config = {
            {"host", test_host_},
            {"port", test_port_}
        };

        ASSERT_TRUE(backend->init(config));
        EXPECT_TRUE(backend->is_connected());

        int vol = backend->get_volume().value_or(-1);
        EXPECT_GE(vol, 0);
        EXPECT_LE(vol, 100);

        backend->close();
        wait_for_command(100);
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(DenonConnectionTest, InvalidHost) {
    SKIP_IF_NO_HARDWARE();
    backend_ = create_denon_ip_backend();
    std::map<std::string, std::string> config = {
        {"host", "192.168.255.255"},  // Invalid/unreachable
        {"port", "23"}
    };

    // Should fail gracefully
    EXPECT_FALSE(backend_->init(config));
    EXPECT_FALSE(backend_->is_connected());
}

TEST_F(DenonConnectionTest, InvalidPort) {
    SKIP_IF_NO_HARDWARE();
    if (!hardware_available_) {
        GTEST_SKIP() << "Hardware tests skipped";
    }

    backend_ = create_denon_ip_backend();
    std::map<std::string, std::string> config = {
        {"host", test_host_},
        {"port", "9999"}  // Wrong port
    };

    // Should fail gracefully
    EXPECT_FALSE(backend_->init(config));
    EXPECT_FALSE(backend_->is_connected());
}

TEST_F(DenonConnectionTest, MissingConfig) {
    SKIP_IF_NO_HARDWARE();
    backend_ = create_denon_ip_backend();
    std::map<std::string, std::string> config;  // Empty config

    // Should fail gracefully
    EXPECT_FALSE(backend_->init(config));
}

// ============================================================================
// Main
// ============================================================================

// ============================================================================
// Unsolicited push path (volume-sync feed): event_fd() + pump_events()
// ============================================================================

// Wait up to `ms` for the backend's push fd to become readable, then consume.
static AlsaAVR::AvrPush drain_push(AlsaAVR::Backend* be, int ms) {
    int fd = be->event_fd();
    if (fd >= 0) {
        struct timeval tv { ms / 1000, (ms % 1000) * 1000 };
        fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
        select(fd + 1, &r, nullptr, nullptr, &tv);
    }
    return be->pump_events();
}

// An external volume change on the receiver (remote/CEC) is pushed to our telnet
// session; pump_events() must report it so the daemon can reflect it to AirPlay.
TEST_F(DenonConnectionTest, PumpEventsReportsExternalVolumePush) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    ASSERT_GE(backend_->event_fd(), 0);
    drain_push(backend_.get(), 200);                 // clear anything pending

    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XPUSHMV 40"));
    AlsaAVR::AvrPush p = drain_push(backend_.get(), 800);
    ASSERT_TRUE(p.volume.has_value());
    int hwmax = backend_->get_hardware_max_volume();
    int expect = (40 * 100 + hwmax / 2) / hwmax;     // MV40 -> percent
    EXPECT_LE(std::abs(*p.volume - expect), 2);
}

// An external mute change is likewise reported.
TEST_F(DenonConnectionTest, PumpEventsReportsExternalMutePush) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    drain_push(backend_.get(), 200);

    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XPUSHMU 1"));
    AlsaAVR::AvrPush p = drain_push(backend_.get(), 800);
    ASSERT_TRUE(p.mute.has_value());
    EXPECT_TRUE(*p.mute);

    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XPUSHMU 0"));
    p = drain_push(backend_.get(), 800);
    ASSERT_TRUE(p.mute.has_value());
    EXPECT_FALSE(*p.mute);
}

// A push arriving must not corrupt a subsequent query response (the shared
// line buffer must keep the two straight).
TEST_F(DenonConnectionTest, QueryStillWorksAfterPush) {
    SKIP_IF_NO_HARDWARE();
    ASSERT_TRUE(init_backend());
    ASSERT_TRUE(backend_->set_volume_absolute(30));
    ASSERT_TRUE(raw_avr_command(test_host_, test_port_, "XPUSHMV 55"));
    wait_for_command(300);
    auto v = backend_->get_volume();                 // must return a sane percent
    ASSERT_TRUE(v.has_value());
    EXPECT_GE(*v, 0); EXPECT_LE(*v, 100);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  Denon Connection Integration Tests\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    std::cout << "To run tests with real hardware, set:\n";
    std::cout << "  export DENON_TEST_HOST=192.168.x.x\n";
    std::cout << "  export DENON_TEST_PORT=23\n";
    std::cout << "\n";
    std::cout << "Without these, hardware tests will be skipped.\n";
    std::cout << "\n";

    return RUN_ALL_TESTS();
}

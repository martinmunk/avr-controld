// Broker integration tests: exercise the real SocketClientBackend against an
// auto-spawned avr-controld talking to the mock Denon. No hardware needed.
//
// The harness (run_broker_tests.sh) provides:
//   MOCK_HOST / MOCK_PORT   - the mock Denon (started by the harness)
//   AVR_SOCKET_DIR          - a writable temp dir for the daemon socket
//   AVR_CONTROLD_BIN        - path to the freshly built avr-controld
// so that create_denon_ip_backend()->init() spawns the daemon against the mock.

#include <gtest/gtest.h>
#include "../../include/backend.hpp"

#include <cstdlib>
#include <cmath>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <string>

using namespace AlsaAVR;

// Provided by socket_client.cpp (linked in): returns a SocketClientBackend.
namespace AlsaAVR { std::unique_ptr<Backend> create_denon_ip_backend(); }

static std::map<std::string, std::string> mock_cfg() {
    std::map<std::string, std::string> c;
    const char* h = std::getenv("MOCK_HOST"); c["host"] = h ? h : "127.0.0.1";
    const char* p = std::getenv("MOCK_PORT"); c["port"] = p ? p : "23333";
    c["input"] = "MPLAY";
    return c;
}

static std::unique_ptr<Backend> make_client() {
    auto b = create_denon_ip_backend();
    b->init(mock_cfg());   // connects (auto-spawns the daemon on first client)
    return b;
}

class Broker : public ::testing::Test {
protected:
    std::unique_ptr<Backend> be;
    void SetUp() override {
        be = create_denon_ip_backend();
        ASSERT_TRUE(be->init(mock_cfg())) << "client init/connect/spawn failed";
        ASSERT_TRUE(be->is_connected()) << "daemon reports it is not connected to the mock";
    }
};

TEST_F(Broker, Capabilities) {
    auto c = be->capabilities();
    EXPECT_TRUE(c.supports_absolute);
    EXPECT_EQ(c.min_volume, 0);
    EXPECT_EQ(c.max_volume, 100);
    EXPECT_GT(be->get_hardware_max_volume(), 0);
    EXPECT_FALSE(be->get_available_inputs().empty());
    EXPECT_EQ(be->name(), std::string("Denon IP"));
}

TEST_F(Broker, VolumeRoundTrip) {
    ASSERT_TRUE(be->set_volume_absolute(42));
    auto v = be->get_volume();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(std::abs(*v - 42), 1);        // +/-1 for the %<->MV scale rounding
    ASSERT_TRUE(be->set_volume_absolute(8));
    v = be->get_volume();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(std::abs(*v - 8), 1);
}

TEST_F(Broker, Power) {
    ASSERT_TRUE(be->set_power(true));
    auto p = be->get_power();
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(*p);
}

TEST_F(Broker, InputSelect) {
    ASSERT_TRUE(be->set_input("TV"));
    auto in = be->get_current_input();
    if (in.has_value()) EXPECT_EQ(*in, "TV");
}

// The whole point of the broker: two independent client handles share the SAME
// daemon and connection, so state set through one is visible through the other.
TEST_F(Broker, SharedStateAcrossClients) {
    auto b2 = make_client();
    ASSERT_TRUE(b2->is_connected());
    ASSERT_TRUE(be->set_volume_absolute(31));
    auto v = b2->get_volume();
    ASSERT_TRUE(v.has_value());
    EXPECT_LE(std::abs(*v - 31), 1);
}

// Many clients hammering concurrently must never get garbage or failed replies:
// the single-threaded daemon serializes the shared wire (this is exactly the
// per-process contention the broker removes).
TEST_F(Broker, ConcurrentClientsNoCorruption) {
    std::atomic<int> fails{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&, i] {
            auto b = create_denon_ip_backend();
            if (!b->init(mock_cfg())) { fails++; return; }
            for (int j = 0; j < 25; ++j) {
                if (!b->set_volume_absolute((i * 9 + j) % 80)) { ++fails; return; }
                auto v = b->get_volume();
                if (!v || *v < 0 || *v > 100) { ++fails; return; }  // valid, never garbage
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(fails.load(), 0);
}

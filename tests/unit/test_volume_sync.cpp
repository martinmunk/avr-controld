// Unit tests for the pure volume mapping + bidirectional sync state machine.
#include <gtest/gtest.h>
#include "../../include/volume_map.hpp"
#include "../../include/volume_sync.hpp"

using namespace AlsaAVR;

// ---------------- VolumeMap ----------------
TEST(VolumeMap, EndpointsAndMidpoint) {
    VolumeMap m;                     // -30..0 dB
    EXPECT_EQ(m.db_to_pct(0.0), 100);
    EXPECT_EQ(m.db_to_pct(-30.0), 0);
    EXPECT_EQ(m.db_to_pct(-15.0), 50);
    EXPECT_EQ(m.db_to_pct(VolumeMap::kMuteDb), 0);
    EXPECT_TRUE(VolumeMap::is_mute_db(-144.0));
    EXPECT_FALSE(VolumeMap::is_mute_db(-30.0));
}

TEST(VolumeMap, ClampsOutOfRange) {
    VolumeMap m;
    EXPECT_EQ(m.db_to_pct(5.0), 100);
    EXPECT_EQ(m.db_to_pct(-99.0), 0);
}

TEST(VolumeMap, SafetyCeiling) {
    VolumeMap m; m.max_pct = 60;
    EXPECT_EQ(m.db_to_pct(0.0), 100);          // uncapped
    EXPECT_EQ(m.db_to_pct_capped(0.0), 60);    // capped
    EXPECT_EQ(m.db_to_pct_capped(-30.0), 0);
}

TEST(VolumeMap, RoundTripMonotonic) {
    VolumeMap m;
    int prev = -1;
    for (int pct = 0; pct <= 100; ++pct) {
        double db = m.pct_to_db(pct);
        int back = m.db_to_pct(db);
        EXPECT_LE(std::abs(back - pct), 1) << "pct=" << pct;
        EXPECT_GE(back, prev);               // monotonic non-decreasing
        prev = back;
    }
}

// ---------------- VolumeSync: AirPlay -> Denon ----------------
TEST(VolumeSync, AirplayDrivesDenonThenSuppressesEcho) {
    VolumeSync s(VolumeMap{}, /*coalesce_ms*/0);
    // Phone sets -15 dB -> ~50%.
    auto a = s.on_airplay_db(-15.0, 1000);
    ASSERT_TRUE(a.set_denon_pct.has_value());
    EXPECT_EQ(*a.set_denon_pct, 50);
    // The receiver echoes MV50 back -> must be swallowed (no write to airplay).
    auto e = s.on_denon_pct(50, 1005);
    EXPECT_TRUE(e.empty());
}

TEST(VolumeSync, DenonExternalChangeReflectsToAirplayThenSuppressesEcho) {
    VolumeSync s(VolumeMap{}, 0);
    // External: remote sets receiver to 75%.
    auto a = s.on_denon_pct(75, 2000);
    ASSERT_TRUE(a.write_airplay_db.has_value());
    EXPECT_NEAR(*a.write_airplay_db, VolumeMap{}.pct_to_db(75), 0.01);
    // shairport emits that .Volume back -> swallow it, no Denon set.
    auto e = s.on_airplay_db(*a.write_airplay_db, 2005);
    EXPECT_TRUE(e.empty());
}

TEST(VolumeSync, NoOpWhenAlreadyAtTarget) {
    VolumeSync s(VolumeMap{}, 0);
    s.on_airplay_db(-15.0, 1000);        // -> 50, sent
    auto again = s.on_airplay_db(-15.0, 1100);
    EXPECT_TRUE(again.empty());          // already there
}

// ---------------- Coalescing ----------------
TEST(VolumeSync, CoalescesRapidRampToLatest) {
    VolumeSync s(VolumeMap{}, /*coalesce_ms*/150);
    auto a0 = s.on_airplay_db(-30.0, 0);        // 0% : first send goes immediately
    ASSERT_TRUE(a0.set_denon_pct.has_value());
    EXPECT_EQ(*a0.set_denon_pct, 0);
    // Rapid drag within the window: each returns nothing now, latest is pending.
    EXPECT_TRUE(s.on_airplay_db(-24.0, 20).empty());   // 20%
    EXPECT_TRUE(s.on_airplay_db(-18.0, 40).empty());   // 40%
    EXPECT_TRUE(s.on_airplay_db(-9.0, 60).empty());    // 70%
    ASSERT_TRUE(s.next_flush_ms().has_value());
    // Before the window elapses: still nothing.
    EXPECT_TRUE(s.tick(100).empty());
    // After the window: flush ONLY the latest target (70%), not the intermediates.
    auto f = s.tick(160);
    ASSERT_TRUE(f.set_denon_pct.has_value());
    EXPECT_EQ(*f.set_denon_pct, 70);
}

// ---------------- Mute ----------------
TEST(VolumeSync, AirplayMuteMutesDenon) {
    VolumeSync s(VolumeMap{}, 0);
    auto a = s.on_airplay_db(VolumeMap::kMuteDb, 1000);
    ASSERT_TRUE(a.set_denon_mute.has_value());
    EXPECT_TRUE(*a.set_denon_mute);
    EXPECT_FALSE(a.set_denon_pct.has_value());        // MV left alone
    // Denon echoes MUON -> swallow.
    EXPECT_TRUE(s.on_denon_mute(true, 1005).empty());
}

TEST(VolumeSync, DenonMuteReflectsToAirplay) {
    VolumeSync s(VolumeMap{}, 0);
    s.on_denon_pct(50, 900);                          // establish a level
    auto a = s.on_denon_mute(true, 1000);
    ASSERT_TRUE(a.write_airplay_db.has_value());
    EXPECT_TRUE(VolumeMap::is_mute_db(*a.write_airplay_db));
    // Unmute -> restore slider to the level.
    auto u = s.on_denon_mute(false, 1100);
    ASSERT_TRUE(u.write_airplay_db.has_value());
    EXPECT_NEAR(*u.write_airplay_db, VolumeMap{}.pct_to_db(50), 0.01);
}

TEST(VolumeSync, UnmuteAndSetInOneAirplayUpdate) {
    VolumeSync s(VolumeMap{}, 0);
    s.on_airplay_db(VolumeMap::kMuteDb, 1000);        // muted
    auto a = s.on_airplay_db(-6.0, 1100);             // slider back up to 80%
    ASSERT_TRUE(a.set_denon_mute.has_value());
    EXPECT_FALSE(*a.set_denon_mute);                  // unmute
    ASSERT_TRUE(a.set_denon_pct.has_value());
    EXPECT_EQ(*a.set_denon_pct, 80);
}

// ---------------- No echo storm under alternating reports ----------------
TEST(VolumeSync, NoStormWhenBothSidesConverge) {
    VolumeSync s(VolumeMap{}, 0);
    auto a = s.on_airplay_db(-12.0, 0);               // -> 60%
    ASSERT_TRUE(a.set_denon_pct.has_value());
    int sends = 0, writes = 0;
    // Simulate the echo bouncing a few times; must settle immediately.
    double db = VolumeMap{}.pct_to_db(*a.set_denon_pct);
    for (int i = 0; i < 5; ++i) {
        auto e1 = s.on_denon_pct(60, 10 + i);   if (e1.write_airplay_db) writes++;
        auto e2 = s.on_airplay_db(db, 20 + i);  if (e2.set_denon_pct)   sends++;
    }
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(sends, 0);
}

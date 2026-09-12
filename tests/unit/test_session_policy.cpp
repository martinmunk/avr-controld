// Unit tests for SessionPolicy: the pure play/pause -> AVR-power decision logic
// behind the shairport-sync D-Bus integration. No D-Bus or hardware involved.

#include <gtest/gtest.h>
#include "../../include/avr_session_policy.hpp"

using namespace AlsaAVR;

TEST(SessionPolicy, PlayPowersOnOnceThenDebounces) {
    SessionPolicy p(120);
    // First Play powers on + selects input.
    EXPECT_EQ(p.feed(SessionSignal::Play, 0), AvrAction::PowerOnSelectInput);
    EXPECT_TRUE(p.believed_on());
    // Repeated Play signals (shairport re-emits) do not re-thrash the AVR.
    EXPECT_EQ(p.feed(SessionSignal::Play, 1), AvrAction::None);
    EXPECT_EQ(p.feed(SessionSignal::Play, 2), AvrAction::None);
}

TEST(SessionPolicy, PauseDoesNotPowerOffImmediately) {
    SessionPolicy p(120);
    p.feed(SessionSignal::Play, 0);
    // Pause arms the grace timer but does not act now.
    EXPECT_EQ(p.feed(SessionSignal::Pause, 10), AvrAction::None);
    ASSERT_TRUE(p.next_deadline().has_value());
    EXPECT_EQ(*p.next_deadline(), 10 + 120);
    // Before the grace elapses, tick() does nothing.
    EXPECT_EQ(p.tick(100), AvrAction::None);
}

TEST(SessionPolicy, ResumeWithinGraceCancelsPowerOff) {
    SessionPolicy p(120);
    p.feed(SessionSignal::Play, 0);
    p.feed(SessionSignal::Pause, 10);
    // Resume (Play) before the deadline cancels the pending power-off...
    EXPECT_EQ(p.feed(SessionSignal::Play, 30), AvrAction::None); // already "on"
    EXPECT_FALSE(p.next_deadline().has_value());
    // ...so the old deadline never fires.
    EXPECT_EQ(p.tick(200), AvrAction::None);
    EXPECT_TRUE(p.believed_on());
}

TEST(SessionPolicy, StopThenGraceElapsesPowersOffExactlyOnce) {
    SessionPolicy p(120);
    p.feed(SessionSignal::Play, 0);
    EXPECT_EQ(p.feed(SessionSignal::Stop, 10), AvrAction::None);
    // At the deadline, power off fires once.
    EXPECT_EQ(p.tick(10 + 120), AvrAction::PowerOff);
    EXPECT_FALSE(p.believed_on());
    // ...and not again.
    EXPECT_EQ(p.tick(10 + 240), AvrAction::None);
}

TEST(SessionPolicy, SessionDownAlsoSchedulesPowerOff) {
    SessionPolicy p(60);
    p.feed(SessionSignal::Play, 0);
    EXPECT_EQ(p.feed(SessionSignal::SessionDown, 5), AvrAction::None);
    ASSERT_TRUE(p.next_deadline().has_value());
    EXPECT_EQ(p.tick(65), AvrAction::PowerOff);
}

TEST(SessionPolicy, SessionUpAloneDoesNotPowerOn) {
    SessionPolicy p(120);
    // A client merely connecting (no playback) must not power the AVR on.
    EXPECT_EQ(p.feed(SessionSignal::SessionUp, 0), AvrAction::None);
    EXPECT_FALSE(p.believed_on());
    EXPECT_FALSE(p.next_deadline().has_value());
}

TEST(SessionPolicy, PauseWhileIdleIsNoop) {
    SessionPolicy p(120);
    // Never played; a stray Pause/Stop must not schedule anything.
    EXPECT_EQ(p.feed(SessionSignal::Pause, 0), AvrAction::None);
    EXPECT_FALSE(p.next_deadline().has_value());
    EXPECT_EQ(p.tick(1000), AvrAction::None);
}

TEST(SessionPolicy, FirstPauseDeadlineIsKeptAcrossFurtherStops) {
    SessionPolicy p(120);
    p.feed(SessionSignal::Play, 0);
    p.feed(SessionSignal::Pause, 10);           // deadline = 130
    // A subsequent Stop/SessionDown must not push the deadline out repeatedly.
    p.feed(SessionSignal::Stop, 20);
    ASSERT_TRUE(p.next_deadline().has_value());
    EXPECT_EQ(*p.next_deadline(), 130);
}

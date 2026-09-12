#pragma once
//
// SessionPolicy: the pure play/pause -> AVR-power decision logic for the
// shairport-sync D-Bus integration, with NO D-Bus or Backend dependencies so it
// can be unit-tested in isolation.
//
// It turns a stream of shairport session signals (play / pause / stop / session
// up / session down) into AVR actions, debouncing so that:
//   * a real play powers the AVR on and selects the configured input (once, not
//     on every repeated "playing" signal);
//   * a pause or stop does NOT immediately power off -- it arms a grace timer,
//     so a brief pause or a gap between tracks does not thrash the receiver;
//   * only when the grace elapses with no resume does it power the AVR off.
//
// Time is passed in as a monotonic second count so tests are deterministic.

#include <cstdint>
#include <optional>

namespace AlsaAVR {

// What shairport just told us (derived from D-Bus PlayerState / Active).
enum class SessionSignal {
    Play,         // PlayerState -> Playing
    Pause,        // PlayerState -> Paused
    Stop,         // PlayerState -> Stopped
    SessionUp,    // Active -> true  (a client connected; not yet playing)
    SessionDown,  // Active -> false (client gone)
};

// What the daemon should do to the AVR.
enum class AvrAction {
    None,               // nothing to do
    PowerOnSelectInput, // ensure power on + select configured input, then verify
    PowerOff,           // power off (do NOT touch input -- CEC-safe)
};

class SessionPolicy {
public:
    explicit SessionPolicy(int off_grace_sec = 120) : off_grace_sec_(off_grace_sec) {}

    // Feed a signal observed at monotonic time `now_sec`. Returns the action to
    // take right now.
    AvrAction feed(SessionSignal sig, int64_t now_sec) {
        switch (sig) {
            case SessionSignal::Play:
                off_deadline_.reset();          // cancel any pending power-off
                if (on_) return AvrAction::None; // already on: don't re-thrash
                on_ = true;
                return AvrAction::PowerOnSelectInput;

            case SessionSignal::Pause:
            case SessionSignal::Stop:
            case SessionSignal::SessionDown:
                if (!on_ && !off_deadline_) return AvrAction::None; // already idle
                // Arm (or keep) the grace timer; power-off happens in tick().
                if (!off_deadline_)
                    off_deadline_ = now_sec + off_grace_sec_;
                return AvrAction::None;

            case SessionSignal::SessionUp:
                // A client connected but is not playing yet. Do nothing until a
                // Play arrives, so merely browsing to the device never powers
                // the AVR on.
                return AvrAction::None;
        }
        return AvrAction::None;
    }

    // Call when the grace timer may have elapsed. Returns PowerOff exactly once
    // when the deadline passes with no intervening Play.
    AvrAction tick(int64_t now_sec) {
        if (off_deadline_ && now_sec >= *off_deadline_) {
            off_deadline_.reset();
            on_ = false;
            return AvrAction::PowerOff;
        }
        return AvrAction::None;
    }

    // Earliest monotonic second at which tick() would act, if a power-off is
    // pending; used to bound the daemon's poll() timeout. nullopt = no timer.
    std::optional<int64_t> next_deadline() const { return off_deadline_; }

    // True once we have decided the AVR should be on (for tests / introspection).
    bool believed_on() const { return on_; }

private:
    int off_grace_sec_;
    bool on_ = false;
    std::optional<int64_t> off_deadline_;
};

} // namespace AlsaAVR

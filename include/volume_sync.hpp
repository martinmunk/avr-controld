#pragma once
//
// VolumeSync: pure, dependency-free state machine that keeps the AirPlay volume
// (shairport-sync's D-Bus `.Volume`) and the AVR's volume/mute in step, in BOTH
// directions, without echoing a change back to the side it came from (which is
// exactly what would otherwise oscillate / feel jerky).
//
// It is the single hub the daemon feeds:
//   * on_airplay_db(db, now)  -- shairport reported a new AirPlay volume
//   * on_denon_pct(pct, now)  -- the receiver reported a new volume (push/read)
//   * on_denon_mute(m, now)   -- the receiver reported a new mute state
//   * tick(now)               -- flush a coalesced pending Denon set
// and returns an Action describing what to do to the OTHER side.
//
// Echo suppression: whenever we emit a change toward a side, we remember the
// value we pushed. The very next report from that side that matches (within a
// tolerance) is consumed as our own echo and NOT propagated back. It is a
// one-shot, so a genuine repeat of the same value later still propagates.
//
// Coalescing: the AirPlay->Denon direction rate-limits, always chasing the
// latest target, so a fast slider drag becomes a smooth chase to where it
// landed instead of a queue of stale MV commands.
//
// Time is passed in as monotonic milliseconds so tests are deterministic.

#include <cstdint>
#include <cstdlib>
#include <optional>
#include "volume_map.hpp"

namespace AlsaAVR {

class VolumeSync {
public:
    struct Action {
        std::optional<int>    set_denon_pct;    // set receiver volume (0-100, capped)
        std::optional<bool>   set_denon_mute;   // set receiver mute
        std::optional<double> write_airplay_db; // write shairport .Volume (dB; -144 = mute)
        bool empty() const { return !set_denon_pct && !set_denon_mute && !write_airplay_db; }
    };

    VolumeSync(VolumeMap map, int coalesce_ms = 150,
               int echo_tol_pct = 2, double echo_tol_db = 0.6)
        : map_(map), coalesce_ms_(coalesce_ms),
          echo_tol_pct_(echo_tol_pct), echo_tol_db_(echo_tol_db) {}

    const VolumeMap& map() const { return map_; }

    // ---- AirPlay side reported a new volume (dB) ------------------------
    Action on_airplay_db(double db, int64_t now_ms) {
        // Echo of a value we just wrote up to shairport? Consume, don't bounce.
        if (expect_airplay_db_ && std::abs(db - *expect_airplay_db_) <= echo_tol_db_) {
            expect_airplay_db_.reset();
            last_airplay_db_ = db;
            return {};
        }
        expect_airplay_db_.reset();
        last_airplay_db_ = db;

        Action a;
        if (VolumeMap::is_mute_db(db)) {
            // Phone muted -> mute the receiver (leave its MV as is).
            if (denon_mute_.value_or(false) != true) {
                a.set_denon_mute = true;
                note_pushed_denon_mute(true);
            }
            return a;
        }
        // Phone at a level -> unmute if needed, then chase the level.
        if (denon_mute_.value_or(false) == true) {
            a.set_denon_mute = false;
            note_pushed_denon_mute(false);
        }
        int target = map_.db_to_pct_capped(db);
        auto flush = chase_denon(target, now_ms);
        if (flush) a.set_denon_pct = *flush;
        return a;
    }

    // ---- Receiver reported a new volume (percent) -----------------------
    Action on_denon_pct(int pct, int64_t now_ms) {
        (void)now_ms;
        if (expect_denon_pct_ && std::abs(pct - *expect_denon_pct_) <= echo_tol_pct_) {
            expect_denon_pct_.reset();       // our own echo
            last_denon_pct_ = pct;
            return {};
        }
        expect_denon_pct_.reset();
        last_denon_pct_ = pct;

        // External change -> reflect up to the phone (unless the phone already
        // shows this level, within a step).
        double db = map_.pct_to_db(pct);
        if (last_airplay_db_ && !VolumeMap::is_mute_db(*last_airplay_db_) &&
            std::abs(db - *last_airplay_db_) <= echo_tol_db_) {
            return {};
        }
        Action a; a.write_airplay_db = db;
        note_pushed_airplay(db);
        return a;
    }

    // ---- Receiver reported a new mute state -----------------------------
    Action on_denon_mute(bool m, int64_t now_ms) {
        (void)now_ms;
        if (expect_denon_mute_ && *expect_denon_mute_ == m) {
            expect_denon_mute_.reset();
            denon_mute_ = m;
            return {};
        }
        expect_denon_mute_.reset();
        bool was = denon_mute_.value_or(!m);
        denon_mute_ = m;
        if (was == m) return {};             // no real change

        Action a;
        if (m) {
            a.write_airplay_db = VolumeMap::kMuteDb;   // phone shows muted
            note_pushed_airplay(VolumeMap::kMuteDb);
        } else {
            // Unmute -> restore the phone slider to the receiver's level.
            double db = map_.pct_to_db(last_denon_pct_.value_or(map_.db_to_pct(last_airplay_db_.value_or(0.0))));
            a.write_airplay_db = db;
            note_pushed_airplay(db);
        }
        return a;
    }

    // ---- Flush a coalesced Denon set ------------------------------------
    Action tick(int64_t now_ms) {
        Action a;
        if (pending_pct_ && now_ms >= pending_flush_ms_) {
            int p = *pending_pct_;
            pending_pct_.reset();
            emit_denon(p, now_ms);
            a.set_denon_pct = p;
        }
        return a;
    }

    std::optional<int64_t> next_flush_ms() const {
        if (pending_pct_) return pending_flush_ms_;
        return std::nullopt;
    }

    // Introspection for tests.
    std::optional<int>  last_denon_pct()  const { return last_denon_pct_; }
    std::optional<double> last_airplay_db() const { return last_airplay_db_; }

private:
    // Decide whether to send a Denon volume now or coalesce. Returns the pct to
    // send now, or nullopt if it was coalesced (tick() will flush it).
    std::optional<int> chase_denon(int target, int64_t now_ms) {
        if (last_sent_denon_pct_ && *last_sent_denon_pct_ == target) {
            pending_pct_.reset();            // nothing new to do
            return std::nullopt;
        }
        if (last_denon_send_ms_ < 0 || now_ms - last_denon_send_ms_ >= coalesce_ms_) {
            pending_pct_.reset();
            emit_denon(target, now_ms);
            return target;
        }
        // Too soon: remember the latest target and schedule a flush.
        pending_pct_ = target;
        pending_flush_ms_ = last_denon_send_ms_ + coalesce_ms_;
        return std::nullopt;
    }

    void emit_denon(int pct, int64_t now_ms) {
        last_denon_send_ms_ = now_ms;
        last_sent_denon_pct_ = pct;
        note_pushed_denon_pct(pct);
    }

    void note_pushed_denon_pct(int pct)  { expect_denon_pct_ = pct;  last_denon_pct_ = pct; }
    void note_pushed_denon_mute(bool m)  { expect_denon_mute_ = m;   denon_mute_ = m; }
    void note_pushed_airplay(double db)  { expect_airplay_db_ = db;  last_airplay_db_ = db; }

    VolumeMap map_;
    int coalesce_ms_;
    int echo_tol_pct_;
    double echo_tol_db_;

    // Last observed state of each side.
    std::optional<int>    last_denon_pct_;
    std::optional<bool>   denon_mute_;
    std::optional<double> last_airplay_db_;

    // One-shot echo expectations.
    std::optional<int>    expect_denon_pct_;
    std::optional<bool>   expect_denon_mute_;
    std::optional<double> expect_airplay_db_;

    // Coalescing state for the AirPlay->Denon direction.
    std::optional<int> pending_pct_;
    int64_t pending_flush_ms_ = 0;
    int64_t last_denon_send_ms_ = -1;
    std::optional<int> last_sent_denon_pct_;
};

} // namespace AlsaAVR

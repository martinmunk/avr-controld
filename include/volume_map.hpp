#pragma once
//
// VolumeMap: pure, dependency-free conversion between the AirPlay volume scale
// (shairport-sync's D-Bus `.Volume`, a dB value) and the daemon's 0-100 percent
// scale (which the Denon backend then maps to the receiver's MV steps).
//
// AirPlay volume is a dB figure in [-30.0, 0.0] where 0 dB is maximum and
// -30 dB is minimum; the special value -144.0 means muted. We map that linearly
// onto 0-100 percent. A safety ceiling (`max_pct`) clamps the percent we will
// ever push toward the receiver, so a phone at full slider can never drive the
// AVR to a dangerous level.
//
// Kept header-only and free of I/O so it can be unit-tested directly.

#include <cmath>
#include <algorithm>

namespace AlsaAVR {

struct VolumeMap {
    double airplay_min_db = -30.0;   // AirPlay dB that maps to 0%
    int    max_pct        = 100;     // safety ceiling for the ->Denon direction

    static constexpr double kMuteDb = -144.0;   // AirPlay "muted" sentinel

    // A dB at/below this is treated as "muted" rather than a level.
    static bool is_mute_db(double db) { return db <= -100.0; }

    // AirPlay dB -> percent (0-100), NOT capped. Mute maps to 0.
    int db_to_pct(double db) const {
        if (is_mute_db(db)) return 0;
        double lo = airplay_min_db, hi = 0.0;
        if (db < lo) db = lo;
        if (db > hi) db = hi;
        double frac = (db - lo) / (hi - lo);          // 0..1
        int pct = static_cast<int>(std::lround(frac * 100.0));
        return std::clamp(pct, 0, 100);
    }

    // AirPlay dB -> percent, clamped to the safety ceiling. Use this whenever
    // driving the receiver.
    int db_to_pct_capped(double db) const {
        return std::min(db_to_pct(db), max_pct);
    }

    // percent (0-100) -> AirPlay dB in [airplay_min_db, 0].
    double pct_to_db(int pct) const {
        pct = std::clamp(pct, 0, 100);
        double lo = airplay_min_db, hi = 0.0;
        return lo + (hi - lo) * (pct / 100.0);
    }
};

} // namespace AlsaAVR

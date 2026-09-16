"""Volume safety clamp.

This is the last line of defence against the failure that has already happened
once on real hardware: a stray/buggy request driving a receiver to full volume
and blasting the speakers. EVERY path that can set an absolute receiver volume
must route through :func:`clamp_absolute`, and the ceiling must be a proven,
configured value -- never trust the caller.

Kept dependency-free and pure so it is trivially unit-testable in CI with no
hardware and no I/O.
"""

from __future__ import annotations

# Absolute floor/ceiling of the 0-100 volume scale the socket protocol uses.
VOLUME_MIN = 0
VOLUME_MAX = 100


def clamp_ceiling(ceiling: int) -> int:
    """Normalise a configured ceiling into the valid 0-100 range."""
    if ceiling < VOLUME_MIN:
        return VOLUME_MIN
    if ceiling > VOLUME_MAX:
        return VOLUME_MAX
    return int(ceiling)


def clamp_absolute(requested: int, ceiling: int) -> int:
    """Clamp a requested absolute volume to ``[0, min(ceiling, 100)]``.

    ``ceiling`` is the hard cap this daemon will ever set on the receiver.
    100 means "no cap" (normal mode); a lower value is the safety limit used
    while audio may be playing. The result is always a valid 0-100 integer.
    """
    cap = clamp_ceiling(ceiling)
    if requested < VOLUME_MIN:
        return VOLUME_MIN
    if requested > cap:
        return cap
    return int(requested)


# ---- AirPlay volume mapping (phone slider -> receiver) ---------------------
# shairport reports the sender's AirPlay volume in dB over D-Bus
# (RemoteControl.AirplayVolume). It ranges roughly [AIRPLAY_MIN_DB, 0]; a special
# very-negative value means muted. Map that to the 0-100 receiver scale.
AIRPLAY_MIN_DB = -30.0
AIRPLAY_MUTE_DB = -144.0


def is_airplay_mute(db: float) -> bool:
    """shairport signals mute as a large negative dB (kMuteDb = -144)."""
    return db <= -140.0


def airplay_db_to_pct(db: float, min_db: float = AIRPLAY_MIN_DB,
                      max_db: float = 0.0) -> int:
    """Map an AirPlay dB level to a 0-100 percent (linear over [min_db, max_db])."""
    if max_db <= min_db:
        return 0
    if db <= min_db:
        return 0
    if db >= max_db:
        return 100
    return round((db - min_db) / (max_db - min_db) * 100)


def airplay_pct_to_db(pct: int, min_db: float = AIRPLAY_MIN_DB,
                      max_db: float = 0.0) -> float:
    """Inverse of :func:`airplay_db_to_pct`: a 0-100 percent back to AirPlay dB.

    Used for reverse volume sync (receiver remote -> phone slider): the receiver's
    current volume percent is mapped back to the AirPlay dB scale and written to
    shairport via ``SetAirplayVolume`` so the phone reflects the real level.
    """
    p = max(0, min(100, pct))
    return min_db + (p / 100.0) * (max_db - min_db)


def allow_step_up(current: int | None, ceiling: int) -> bool:
    """Whether a relative volume-up step is allowed given the ceiling.

    Used by relative controls (CEC volume-up, VOLUP) where we cannot clamp an
    absolute target. If the current volume is unknown we allow the step (the
    receiver enforces its own maximum and one step is bounded); if it is known
    and already at/above the ceiling we refuse.
    """
    if current is None:
        return True
    return current < clamp_ceiling(ceiling)

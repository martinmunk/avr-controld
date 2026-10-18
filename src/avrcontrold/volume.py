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


#: Slider-to-receiver curves. "flat" is linear in dB (what the receiver's own
#: dB scale gives). "standard" is shairport-sync's default mixer profile
#: (``vol2attn`` in common.c): the ALPS-potentiometer-like taper that made the
#: lower part of the phone slider usable when shairport drove the receiver
#: through the ALSA mixer plugins -- a third of the slider is ~55%, half is 70%.
#: A receiver migrated off the mixer path onto this daemon keeps its familiar
#: feel with "standard"; "flat" makes the same slider position much quieter.
AIRPLAY_CURVES = ("flat", "standard")

# shairport's three-line taper, normalised: (x where the line starts at y=0,
# y at x=-30 as a fraction of the full range). Scale-free, so the receiver's
# range is irrelevant -- only the slider position on the AirPlay [-30, 0] axis.
_STANDARD_LINES = ((0.0, -0.5), (-5.0, -0.75), (-17.0, -1.0))


def airplay_standard_fraction(x: float) -> float:
    """shairport-sync "standard" profile: AirPlay dB ``x`` in [-30, 0] -> fraction
    of the receiver's range in [0, 1]. Mirrors ``vol2attn`` in common.c with the
    attenuation expressed relative to the range (every line's end point in
    vol2attn is proportional to range_db, so the shape does not depend on it)."""
    if x <= -30.0:
        return 0.0
    if x >= 0.0:
        return 1.0
    attn = 0.0
    for x0, y_end in _STANDARD_LINES:
        if x <= x0:
            t = y_end * (x - x0) / (-30.0 - x0)
            if t < attn:
                attn = t
    return 1.0 + attn


def airplay_db_to_pct(db: float, min_db: float = AIRPLAY_MIN_DB,
                      max_db: float = 0.0, curve: str = "flat") -> int:
    """Map an AirPlay dB level to a 0-100 percent over [min_db, max_db].

    ``curve`` is one of :data:`AIRPLAY_CURVES`: "flat" is linear, "standard" is
    shairport-sync's taper (see :func:`airplay_standard_fraction`). An unknown
    curve raises ValueError so a config typo fails at startup, not at play time.
    """
    if curve not in AIRPLAY_CURVES:
        raise ValueError(f"unknown airplay_curve {curve!r}; use one of {AIRPLAY_CURVES}")
    if max_db <= min_db:
        return 0
    if db <= min_db:
        return 0
    if db >= max_db:
        return 100
    frac = (db - min_db) / (max_db - min_db)
    if curve == "standard":
        frac = airplay_standard_fraction(-30.0 + 30.0 * frac)
    return round(frac * 100)


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

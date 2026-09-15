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

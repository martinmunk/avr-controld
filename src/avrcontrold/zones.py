"""Map an AirPlay endpoint (one shairport-sync instance) to a receiver zone.

The multi-zone Docker setup runs several shairport instances, each in its own
container on its own IP and its own private D-Bus (so they can all own the fixed
name ``org.gnome.ShairportSync``). The daemon watches each bus and routes its
play-state and phone-slider volume to a *zone* of the one receiver:

* ``main``  -> the backend's core volume/mute/power/input.
* ``zone2`` (or any other extra-zone prefix) -> the backend's extra properties
  ``<zone>_volume`` / ``<zone>_mute`` / ``<zone>_power`` / ``<zone>_source``.

This module is pure wiring around a :class:`Backend`: no D-Bus, no threads, no
config parsing. That keeps the volume-safety path (mute detection, dB->percent,
ceiling clamp) identical for every zone and unit-testable with a fake backend.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Callable, Optional

from .backends.base import Backend
from .volume import airplay_db_to_pct, clamp_absolute, is_airplay_mute

log = logging.getLogger(__name__)


@dataclass
class ZoneTarget:
    """One AirPlay endpoint bound to one receiver zone.

    The setters are injected so the same volume/mute/power logic serves both the
    Main core methods and an extra zone's ``set_extra`` calls. ``on_playing`` and
    ``on_volume`` are the callbacks the watcher fires; every absolute volume goes
    through :func:`clamp_absolute` with this zone's ceiling -- the hardware-safety
    cap applies per zone (Zone 2 at 0 dB is very loud too).
    """

    name: str
    set_volume: Callable[[int], bool]
    set_mute: Callable[[bool], bool]
    set_power: Callable[[bool], bool]
    set_source: Callable[[str], bool] | None
    ceiling: int
    source: str = ""
    airplay_min_db: float = -30.0
    # current receiver source name, for input-stolen detection (below).
    read_source: Optional[Callable[[], Optional[str]]] = None
    # internal: remember whether we drove this zone to mute, so we only unmute
    # what we muted.
    _muted: bool = False
    # internal: input-stolen tracking. _input_ok_after gives on_playing a moment
    # for the source select to settle before we check it; _input_dropped avoids
    # dropping the session more than once per steal.
    _input_ok_after: float = 0.0
    _input_dropped: bool = False

    def on_playing(self, playing: bool) -> None:
        """shairport play-state flipped: power the zone on/off and select its
        source when it starts."""
        self.set_power(bool(playing))
        if playing and self.source and self.set_source is not None:
            self.set_source(self.source)
            self._input_ok_after = time.monotonic() + 4.0   # let the select settle
            self._input_dropped = False
        log.info("zone %s: power %s%s", self.name, "ON" if playing else "OFF",
                 f" + source {self.source}" if (playing and self.source) else "")

    def on_volume(self, db: float, playing: bool) -> None:
        """Phone slider moved: drive this zone's volume, clamped to its ceiling.

        Only acts while this endpoint is playing (so an idle instance can't move
        the receiver). shairport's large-negative mute value maps to mute."""
        if not playing:
            return
        if is_airplay_mute(db):
            if not self._muted:
                self.set_mute(True)
                self._muted = True
            return
        if self._muted:
            self.set_mute(False)
            self._muted = False
        pct = clamp_absolute(
            airplay_db_to_pct(db, min_db=self.airplay_min_db), self.ceiling)
        self.set_volume(pct)

    def check_input_stolen(self, connected: bool, drop_fn: Callable[[], bool],
                           now: float | None = None) -> None:
        """If a sender is connected but the receiver's input for this zone has
        been switched away from the zone's AirPlay input (TV selected, another
        source picked on the remote), drop the AirPlay session via ``drop_fn`` so
        the stream isn't left playing silently into a different input.

        Called from the daemon poll loop under the lock.
        """
        if self.read_source is None or not self.source:
            return
        if now is None:
            now = time.monotonic()
        if not connected:
            self._input_dropped = False
            self._input_ok_after = now + 4.0     # grace for the next connect
            return
        if now < self._input_ok_after:
            return                                # let a just-set source settle
        cur = self.read_source()
        if cur is None:
            return
        if cur != self.source:
            if not self._input_dropped:
                self._input_dropped = True
                log.info("zone %s: input is '%s', not '%s' -> dropping AirPlay session",
                         self.name, cur, self.source)
                drop_fn()
        else:
            self._input_dropped = False


def make_zone_target(target: str, backend: Backend, *, name: str, ceiling: int,
                     source: str = "", airplay_min_db: float = -30.0) -> ZoneTarget:
    """Build a :class:`ZoneTarget` for ``target`` on ``backend``.

    ``target == "main"`` binds the backend's core methods; any other value is an
    extra-zone prefix (e.g. ``"zone2"``) bound to
    ``<prefix>_{volume,mute,power,source}`` via ``set_extra``.
    """
    if target == "main":
        return ZoneTarget(
            name=name,
            set_volume=backend.set_volume,
            set_mute=backend.set_mute,
            set_power=backend.set_power,
            set_source=backend.set_input,
            read_source=backend.get_input,
            ceiling=ceiling, source=source, airplay_min_db=airplay_min_db,
        )
    return ZoneTarget(
        name=name,
        set_volume=lambda pct: backend.set_extra(f"{target}_volume", pct),
        set_mute=lambda m: backend.set_extra(f"{target}_mute", m),
        set_power=lambda on: backend.set_extra(f"{target}_power", on),
        set_source=lambda src: backend.set_extra(f"{target}_source", src),
        read_source=lambda: backend.get_extra(f"{target}_source"),
        ceiling=ceiling, source=source, airplay_min_db=airplay_min_db,
    )

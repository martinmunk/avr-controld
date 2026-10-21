"""Backend interface shared by all receiver transports.

Mirrors the C++ ``Backend`` contract (include/backend.hpp) so the Unix-socket
protocol and its clients (ALSA plugins, avr-cec-relay, the MQTT bridge) behave
identically no matter which backend is loaded. State getters return ``None`` when
the value is unknown (not yet queried / unsupported), matching the protocol's
"-" payload.
"""

from __future__ import annotations

import logging
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Callable


class Reconnect:
    """Rate-limits reopen attempts after a transport died.

    A serial adapter that drops off USB (hub reset, cable, re-enumeration as a
    new ttyUSBn) leaves the old descriptor returning EIO forever; the backend
    must close it and open the configured path again. This helper spaces those
    attempts ``interval`` seconds apart so a sweep of a dozen state queries per
    poll does not turn into a reopen storm or a journal flood, and it tells the
    caller which log level a failure deserves: WARNING for the first one after a
    working connection, DEBUG for the repeats until it works again.
    """

    def __init__(self, interval: float = 5.0,
                 clock: Callable[[], float] = time.monotonic):
        self.interval = float(interval)
        self._clock = clock
        self._next = 0.0          # earliest time for the next attempt
        self.failures = 0         # consecutive failures since the last success

    def due(self) -> bool:
        return self._clock() >= self._next

    def failed(self) -> int:
        """Record a failure; returns the log level for reporting it."""
        self.failures += 1
        self._next = self._clock() + self.interval
        return logging.WARNING if self.failures == 1 else logging.DEBUG

    def succeeded(self) -> bool:
        """Record success; returns True if this ends an outage."""
        recovered = self.failures > 0
        self.failures = 0
        self._next = 0.0
        return recovered


@dataclass(frozen=True)
class Capabilities:
    """What a backend can do. Maps to the CAPS protocol line."""
    absolute: bool = False        # can set a specific volume level
    query: bool = False           # can read current volume back
    mute: bool = False            # has mute control
    input_select: bool = False    # has input/source selection
    speaker_level: bool = False   # has per-speaker trim
    standby_timeout: bool = False # has an auto-standby toggle
    power: bool = False           # has power control
    min_volume: int = 0
    max_volume: int = 100


@dataclass(frozen=True)
class ExtraSpec:
    """Describes one extra property a backend exposes beyond the core set, so the
    MQTT bridge can build an HA entity for it generically ("expose all the
    things"). Pure metadata; values are read/written via get_extra/set_extra.
    """
    key: str                       # unique, e.g. "bass", "zone2_volume"
    component: str                 # number | switch | select | sensor
    name: str                      # HA friendly name, e.g. "Bass"
    writable: bool = False
    min: float | None = None       # number
    max: float | None = None
    step: float | None = None
    unit: str | None = None
    options: list[str] | None = None   # select
    mode: str = "box"                  # number display: "box" | "slider" | "auto"
    icon: str | None = None
    category: str | None = None        # HA entity_category: "config" | "diagnostic"
    static: bool = False               # device config/identity: read at announce
                                       # and after a write, not on every poll


@dataclass
class PushEvent:
    """Unsolicited state a receiver reported on its own (remote/CEC/other app).

    Only fields that actually changed are set; the rest stay ``None``.
    """
    volume: int | None = None
    mute: bool | None = None
    power: bool | None = None
    input: str | None = None
    other: dict = field(default_factory=dict)   # receiver-specific Variable=Value

    def empty(self) -> bool:
        return (self.volume is None and self.mute is None
                and self.power is None and self.input is None and not self.other)

    def describe(self) -> str:
        parts = []
        if self.power is not None:
            parts.append("power=" + ("ON" if self.power else "OFF"))
        if self.volume is not None:
            parts.append(f"volume={self.volume}")
        if self.mute is not None:
            parts.append("mute=" + ("ON" if self.mute else "OFF"))
        if self.input is not None:
            parts.append(f"input={self.input}")
        parts += [f"{k}={v}" for k, v in self.other.items()]
        return " ".join(parts)


class Backend(ABC):
    """One real connection to one receiver. Implementations own the transport."""

    #: short human name, e.g. "NAD Serial"
    name: str = "backend"

    @abstractmethod
    def capabilities(self) -> Capabilities: ...

    @abstractmethod
    def is_connected(self) -> bool: ...

    @abstractmethod
    def connect(self) -> bool:
        """Open (or reopen) the connection. Idempotent; returns success."""

    def close(self) -> None:
        """Close the connection. Default: nothing to do."""

    # ---- volume (0-100 scale) -------------------------------------------
    @abstractmethod
    def get_volume(self) -> int | None: ...

    @abstractmethod
    def set_volume(self, level: int) -> bool: ...

    @abstractmethod
    def volume_up(self) -> bool: ...

    @abstractmethod
    def volume_down(self) -> bool: ...

    # ---- mute -----------------------------------------------------------
    @abstractmethod
    def get_mute(self) -> bool | None: ...

    @abstractmethod
    def set_mute(self, muted: bool) -> bool: ...

    # ---- power (optional) ----------------------------------------------
    def get_power(self) -> bool | None:
        return None

    def set_power(self, on: bool) -> bool:
        return False

    # ---- input selection (optional) ------------------------------------
    def get_inputs(self) -> list[str]:
        return []

    def get_input(self) -> str | None:
        return None

    def set_input(self, name: str) -> bool:
        return False

    # ---- extra properties (optional; "expose all the things") ----------
    def extras(self) -> list["ExtraSpec"]:
        """Extra properties this backend exposes (tone/zone2/etc). Default: none."""
        return []

    def get_extra(self, key: str):
        """Current value of an extra property (typed per its component), or None."""
        return None

    def set_extra(self, key: str, value) -> bool:
        return False

    # ---- raw passthrough (optional) ------------------------------------
    def raw(self, command: str) -> str | None:
        """Send a raw protocol command to the receiver and return its raw reply.

        For exploration and device configuration (e.g. NAD Source names/inputs)
        that the typed methods don't cover. None if unsupported/failed. Bypasses
        the volume clamp -- callers must be careful.
        """
        return None

    # ---- real-time push (optional) -------------------------------------
    def poll_events(self) -> PushEvent:
        """Consume any unsolicited state the receiver pushed. Default: none."""
        return PushEvent()

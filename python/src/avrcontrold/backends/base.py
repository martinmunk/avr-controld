"""Backend interface shared by all receiver transports.

Mirrors the C++ ``Backend`` contract (include/backend.hpp) so the Unix-socket
protocol and its clients (ALSA plugins, avr-cec-relay, the MQTT bridge) behave
identically no matter which backend is loaded. State getters return ``None`` when
the value is unknown (not yet queried / unsupported), matching the protocol's
"-" payload.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field


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
    icon: str | None = None


@dataclass
class PushEvent:
    """Unsolicited state a receiver reported on its own (remote/CEC/other app).

    Only fields that actually changed are set; the rest stay ``None``.
    """
    volume: int | None = None
    mute: bool | None = None
    power: bool | None = None
    input: str | None = None

    def empty(self) -> bool:
        return (self.volume is None and self.mute is None
                and self.power is None and self.input is None)


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

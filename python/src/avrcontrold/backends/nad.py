"""NAD backend built on the `nad_receiver` library (RS232, the T7xx protocol).

`nad_receiver` speaks the NAD RS232 protocol and is what Home Assistant's own
NAD integration uses. We do NOT let HA/nad_receiver own the port directly (that
would collide with this daemon); instead this backend is the single owner and
serves everyone through the socket protocol / MQTT bridge.

Volume: the receiver works in dB (float). The socket protocol, ALSA plugins and
CEC relay all speak 0-100, so this backend maps dB<->percent over a configurable
range (default -80..0 dB). Relative up/down use the receiver's native +/- and so
are exact regardless of the map. The receiver object is injectable so the whole
backend is unit-testable with no serial port.
"""

from __future__ import annotations

import logging
from typing import Any, Optional, Protocol

from .base import Backend, Capabilities, ExtraSpec, PushEvent

# Extra NAD properties exposed beyond the core set (over the full RS232 protocol,
# which nad_receiver's command table does not cover -- we drive it via raw).
_NAD_EXTRAS = [
    ExtraSpec("bass", "number", "Bass", writable=True, min=-10, max=10, step=1,
              unit="dB", icon="mdi:music-clef-bass"),
    ExtraSpec("treble", "number", "Treble", writable=True, min=-10, max=10, step=1,
              unit="dB", icon="mdi:music-note"),
    ExtraSpec("zone2_power", "switch", "Zone 2 Power", writable=True, icon="mdi:power"),
    ExtraSpec("zone2_volume", "number", "Zone 2 Volume", writable=True, min=0, max=100,
              step=1, mode="slider", icon="mdi:volume-high"),
    ExtraSpec("zone2_mute", "switch", "Zone 2 Mute", writable=True, icon="mdi:volume-off"),
    ExtraSpec("sleep", "number", "Sleep Timer", writable=True, min=0, max=90, step=15,
              unit="min", icon="mdi:timer"),
    ExtraSpec("listening_mode", "sensor", "Listening Mode", icon="mdi:surround-sound"),
    ExtraSpec("dimmer", "sensor", "Display Dimmer", icon="mdi:brightness-6"),
    ExtraSpec("model", "sensor", "Model", icon="mdi:information"),
    ExtraSpec("version", "sensor", "Firmware", icon="mdi:information"),
]
# key -> (NAD variable, type)
_NAD_EXTRA_VAR = {
    "bass": ("Main.Bass", "int"), "treble": ("Main.Treble", "int"),
    "zone2_power": ("Zone2.Power", "bool"),
    "zone2_mute": ("Zone2.Mute", "bool"), "sleep": ("Main.Sleep", "int"),
    "listening_mode": ("Main.ListeningMode", "str"), "dimmer": ("Main.Dimmer", "str"),
    "model": ("Main.Model", "str"), "version": ("Main.Version", "str"),
}

log = logging.getLogger(__name__)


class NadClient(Protocol):
    """The subset of nad_receiver.NADReceiver this backend uses."""
    def main_power(self, operator: str, value: Optional[str] = None) -> Optional[str]: ...
    def main_mute(self, operator: str, value: Optional[str] = None) -> Optional[str]: ...
    def main_volume(self, operator: str, value: Optional[str] = None) -> Optional[float]: ...
    def main_source(self, operator: str, value: Optional[str] = None) -> Any: ...


def _on(value: Optional[str]) -> Optional[bool]:
    if value is None:
        return None
    return str(value).strip().lower() == "on"


class NADBackend(Backend):
    name = "NAD Serial"

    def __init__(self, device: str, *, receiver: NadClient | None = None,
                 min_db: float = -80.0, max_db: float = 0.0,
                 sources: list[str] | None = None):
        if max_db <= min_db:
            raise ValueError("max_db must be greater than min_db")
        self._device = device
        self._rx = receiver
        self._min_db = float(min_db)
        self._max_db = float(max_db)
        self._sources = list(sources or [])       # manual override (names)
        self._num_to_name: dict[int, str] = {}     # discovered Source<n>.Name
        self._name_to_num: dict[str, int] = {}
        self._enabled_nums: list[int] = []
        self._connected = receiver is not None

    # ---- dB <-> percent -------------------------------------------------
    def _db_to_pct(self, db: float) -> int:
        frac = (db - self._min_db) / (self._max_db - self._min_db)
        return max(0, min(100, round(frac * 100)))

    def _pct_to_db(self, pct: int) -> float:
        pct = max(0, min(100, pct))
        return self._min_db + (pct / 100.0) * (self._max_db - self._min_db)

    # ---- connection -----------------------------------------------------
    def connect(self) -> bool:
        if self._rx is None:
            try:
                from nad_receiver import NADReceiver  # lazy: only needed live
                self._rx = NADReceiver(self._device)
            except Exception as exc:  # pragma: no cover - import/serial failure
                log.error("NAD: cannot open %s: %s", self._device, exc)
                self._connected = False
                return False
        # A query doubles as a liveness check.
        try:
            self._connected = self._rx.main_power("?") is not None
        except Exception as exc:
            log.warning("NAD: probe failed: %s", exc)
            self._connected = False
        if self._connected and not self._sources and not self._num_to_name:
            self._discover_sources()
        return self._connected

    def _discover_sources(self) -> None:
        """Read Source<n>.Name/Enabled so HA gets a source selector with the NAD's
        real names, and set_input can map a name back to its number."""
        for n in range(1, 11):
            name = self._q(f"Source{n}.Name")
            if not name:
                continue
            self._num_to_name[n] = name
            self._name_to_num[name] = n
            en = self._q(f"Source{n}.Enabled")
            if en is None or en.strip().lower() == "yes":
                self._enabled_nums.append(n)
        if self._num_to_name:
            log.info("NAD: discovered %d sources (%d enabled)",
                     len(self._num_to_name), len(self._enabled_nums))

    def is_connected(self) -> bool:
        return self._connected

    def _guard(self):
        if self._rx is None:
            self.connect()
        return self._rx

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=True, mute=True,
                            input_select=bool(self._sources or self._num_to_name),
                            power=True, min_volume=0, max_volume=100)

    # ---- volume ---------------------------------------------------------
    def get_volume(self) -> int | None:
        rx = self._guard()
        if rx is None:
            return None
        try:
            db = rx.main_volume("?")
        except Exception as exc:
            log.warning("NAD: get_volume failed: %s", exc)
            self._connected = False
            return None
        return None if db is None else self._db_to_pct(float(db))

    def set_volume(self, level: int) -> bool:
        rx = self._guard()
        if rx is None:
            return False
        db = self._pct_to_db(level)
        return self._ok(lambda: rx.main_volume("=", f"{db:.0f}"))

    def volume_up(self) -> bool:
        rx = self._guard()
        return bool(rx) and self._ok(lambda: rx.main_volume("+"))

    def volume_down(self) -> bool:
        rx = self._guard()
        return bool(rx) and self._ok(lambda: rx.main_volume("-"))

    # ---- mute -----------------------------------------------------------
    def get_mute(self) -> bool | None:
        rx = self._guard()
        if rx is None:
            return None
        try:
            return _on(rx.main_mute("?"))
        except Exception as exc:
            log.warning("NAD: get_mute failed: %s", exc)
            self._connected = False
            return None

    def set_mute(self, muted: bool) -> bool:
        rx = self._guard()
        return bool(rx) and self._ok(lambda: rx.main_mute("=", "On" if muted else "Off"))

    # ---- power ----------------------------------------------------------
    def get_power(self) -> bool | None:
        rx = self._guard()
        if rx is None:
            return None
        try:
            return _on(rx.main_power("?"))
        except Exception as exc:
            log.warning("NAD: get_power failed: %s", exc)
            self._connected = False
            return None

    def set_power(self, on: bool) -> bool:
        rx = self._guard()
        return bool(rx) and self._ok(lambda: rx.main_power("=", "On" if on else "Off"))

    # ---- input / source -------------------------------------------------
    def get_inputs(self) -> list[str]:
        if self._sources:
            return list(self._sources)
        return [self._num_to_name[n] for n in self._enabled_nums]

    def get_input(self) -> str | None:
        rx = self._guard()
        if rx is None:
            return None
        try:
            v = rx.main_source("?")
        except Exception as exc:
            log.warning("NAD: get_input failed: %s", exc)
            self._connected = False
            return None
        if v is None:
            return None
        try:
            return self._num_to_name.get(int(v), str(v))
        except (ValueError, TypeError):
            return str(v)

    def set_input(self, name: str) -> bool:
        rx = self._guard()
        if rx is None:
            return False
        # Accept a discovered source name, or a raw number/string.
        target = name
        if name in self._name_to_num:
            target = str(self._name_to_num[name])
        return self._ok(lambda: rx.main_source("=", target))

    # ---- extra properties (tone/zone2/sleep/info) -----------------------
    def _q(self, var: str) -> str | None:
        r = self.raw(var + "?")
        return r.split("=", 1)[1] if (r and "=" in r) else None

    def _s(self, var: str, value: str) -> bool:
        r = self.raw(f"{var}={value}")
        return bool(r and "=" in r)

    def extras(self) -> list[ExtraSpec]:
        ex = list(_NAD_EXTRAS)
        names = self.get_inputs()
        if names:
            ex.append(ExtraSpec("zone2_source", "select", "Zone 2 Source",
                                writable=True, options=names, icon="mdi:import"))
        return ex

    def get_extra(self, key: str):
        if key == "zone2_volume":
            v = self._q("Zone2.Volume")
            if v is None:
                return None
            try:
                return self._db_to_pct(float(v))    # same 0-100 mapping as Main
            except ValueError:
                return None
        if key == "zone2_source":
            v = self._q("Zone2.Source")
            if v is None:
                return None
            try:
                return self._num_to_name.get(int(v), str(v))
            except (ValueError, TypeError):
                return str(v)
        spec = _NAD_EXTRA_VAR.get(key)
        if spec is None:
            return None
        var, typ = spec
        v = self._q(var)
        if v is None:
            return None
        if typ == "int":
            try:
                return int(v)
            except ValueError:
                return None
        if typ == "bool":
            return v.strip().lower() == "on"
        return v

    def set_extra(self, key: str, value) -> bool:
        if key == "zone2_volume":
            db = self._pct_to_db(int(value))
            return self._s("Zone2.Volume", f"{db:.0f}")
        if key == "zone2_source":
            num = self._name_to_num.get(str(value))
            return self._s("Zone2.Source", str(num) if num is not None else str(value))
        spec = _NAD_EXTRA_VAR.get(key)
        if spec is None:
            return False
        var, typ = spec
        if typ == "bool":
            val = "On" if value in (True, 1, "1", "on", "ON", "true", "True") else "Off"
        elif typ == "int":
            val = str(int(value))
        else:
            val = str(value)
        return self._s(var, val)

    # ---- raw passthrough (device config / exploration) ------------------
    def raw(self, command: str) -> str | None:
        rx = self._guard()
        if rx is None:
            return None
        try:
            reply = rx.transport.communicate(command)
            self._connected = True
            return reply
        except Exception as exc:
            log.warning("NAD: raw '%s' failed: %s", command, exc)
            self._connected = False
            return None

    # ---- helper ---------------------------------------------------------
    def _ok(self, fn) -> bool:
        try:
            fn()
            self._connected = True
            return True
        except Exception as exc:
            log.warning("NAD: command failed: %s", exc)
            self._connected = False
            return False

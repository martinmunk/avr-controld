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

from .base import Backend, Capabilities, PushEvent

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
        self._sources = list(sources or [])
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
        return self._connected

    def is_connected(self) -> bool:
        return self._connected

    def _guard(self):
        if self._rx is None:
            self.connect()
        return self._rx

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=True, mute=True,
                            input_select=bool(self._sources), power=True,
                            min_volume=0, max_volume=100)

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
        return list(self._sources)

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
        return None if v is None else str(v)

    def set_input(self, name: str) -> bool:
        rx = self._guard()
        return bool(rx) and self._ok(lambda: rx.main_source("=", name))

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

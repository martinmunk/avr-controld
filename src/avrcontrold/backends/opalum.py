"""Opalum backend on the `opalum_receiver` library (RS232, write-only protocol).

The Opalum protocol has NO query commands: power, volume, mute and source are
whatever this daemon last set (``None`` until set). That is why the daemon must
be the single owner of the port -- nothing else can change the device without
the cached state going stale. The receiver object is injectable so the backend
is unit-testable with no serial port.

Mute is ``vol00`` with the previous volume remembered (the library does this);
there is no mute command. ``sta0``/``sta1`` is the device's own auto-standby
timeout, exposed as the ``auto_standby`` extra so the power policy turns it off
when it owns power (see cli.own_receiver_power).
"""

from __future__ import annotations

import logging
from typing import Protocol

from .base import Backend, Capabilities, ExtraSpec

log = logging.getLogger(__name__)

_OPALUM_EXTRAS = [
    ExtraSpec("auto_standby", "switch", "Auto Standby", writable=True,
              icon="mdi:timer-off", category="config"),
]


class OpalumClient(Protocol):
    """The subset of opalum_receiver.OpalumReceiver this backend uses."""
    def set_power(self, on: bool) -> bool: ...
    def get_power(self) -> bool | None: ...
    def set_volume(self, level: int) -> bool: ...
    def volume_up(self) -> bool: ...
    def volume_down(self) -> bool: ...
    def get_volume(self) -> int | None: ...
    def set_mute(self, muted: bool) -> bool: ...
    def get_mute(self) -> bool | None: ...
    def set_source(self, source: str | int) -> bool: ...
    def get_source(self) -> str | None: ...
    def available_sources(self) -> list[str]: ...
    def set_standby_timeout(self, enabled: bool) -> bool: ...
    def get_standby_timeout(self) -> bool | None: ...


class OpalumBackend(Backend):
    name = "Opalum Serial"

    def __init__(self, device: str, *, receiver: OpalumClient | None = None):
        self._device = device
        self._rx = receiver
        self._connected = receiver is not None

    # ---- connection -----------------------------------------------------
    def connect(self) -> bool:
        if self._rx is None:
            try:
                from opalum_receiver import OpalumReceiver  # lazy: only needed live
                # power_on=False: the library powers the device on at open by
                # default (C++ heritage); power is the policy's job here.
                self._rx = OpalumReceiver(self._device, power_on=False)
            except Exception as exc:  # pragma: no cover - import/serial failure
                log.error("Opalum: cannot open %s: %s", self._device, exc)
                self._connected = False
                return False
        # Write-only protocol: no liveness query. Open port == connected; a
        # failed command marks it disconnected and the next call reopens.
        self._connected = True
        return True

    def is_connected(self) -> bool:
        return self._connected

    def _guard(self):
        if self._rx is None:
            self.connect()
        return self._rx

    def _do(self, what: str, fn) -> bool:
        rx = self._guard()
        if rx is None:
            return False
        try:
            ok = bool(fn(rx))
        except Exception as exc:
            log.warning("Opalum: %s failed: %s", what, exc)
            self._connected = False
            return False
        self._connected = True
        if not ok:
            log.warning("Opalum: %s not acknowledged", what)
        return ok

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=False, mute=True,
                            input_select=True, power=True, standby_timeout=True,
                            min_volume=0, max_volume=100)

    # ---- volume ---------------------------------------------------------
    def get_volume(self) -> int | None:
        rx = self._guard()
        return rx.get_volume() if rx else None

    def set_volume(self, level: int) -> bool:
        level = max(0, min(100, int(level)))
        return self._do(f"set_volume({level})", lambda rx: rx.set_volume(level))

    def volume_up(self) -> bool:
        return self._do("volume_up", lambda rx: rx.volume_up())

    def volume_down(self) -> bool:
        return self._do("volume_down", lambda rx: rx.volume_down())

    # ---- mute -----------------------------------------------------------
    def get_mute(self) -> bool | None:
        rx = self._guard()
        return rx.get_mute() if rx else None

    def set_mute(self, muted: bool) -> bool:
        return self._do(f"set_mute({muted})", lambda rx: rx.set_mute(bool(muted)))

    # ---- power ----------------------------------------------------------
    def get_power(self) -> bool | None:
        rx = self._guard()
        return rx.get_power() if rx else None

    def set_power(self, on: bool) -> bool:
        return self._do(f"set_power({on})", lambda rx: rx.set_power(bool(on)))

    # ---- input / source -------------------------------------------------
    def get_inputs(self) -> list[str]:
        rx = self._guard()
        return list(rx.available_sources()) if rx else []

    def get_input(self) -> str | None:
        rx = self._guard()
        return rx.get_source() if rx else None

    def set_input(self, name: str) -> bool:
        try:
            return self._do(f"set_input({name})", lambda rx: rx.set_source(name))
        except ValueError:
            log.warning("Opalum: unknown source %r", name)
            return False

    # ---- extras ---------------------------------------------------------
    def extras(self) -> list[ExtraSpec]:
        return list(_OPALUM_EXTRAS)

    def get_extra(self, key: str):
        rx = self._guard()
        if key == "auto_standby" and rx:
            return rx.get_standby_timeout()
        return None

    def set_extra(self, key: str, value) -> bool:
        if key != "auto_standby":
            return False
        on = value in (True, 1, "1", "on", "ON", "true", "True")
        return self._do(f"set_standby_timeout({on})",
                        lambda rx: rx.set_standby_timeout(on))

    # ---- raw passthrough ------------------------------------------------
    def raw(self, command: str) -> str | None:
        rx = self._guard()
        tr = getattr(rx, "_transport", None)
        if tr is None:
            return None
        try:
            return tr.command(command)
        except Exception as exc:
            log.warning("Opalum: raw '%s' failed: %s", command, exc)
            self._connected = False
            return None

"""In-memory fake backend for offline tests (no hardware, no I/O)."""

from __future__ import annotations

from avrcontrold.backends.base import Backend, Capabilities, ExtraSpec, PushEvent


class FakeBackend(Backend):
    """A fully in-memory receiver. Records calls and simulates state."""

    name = "Fake"

    def __init__(self, *, connected: bool = True, volume: int = 50,
                 mute: bool = False, power: bool = True,
                 inputs: list[str] | None = None, current_input: str | None = None):
        self._connected = connected
        self._volume = volume
        self._mute = mute
        self._power = power
        self._inputs = inputs or ["TV", "CD", "Stream"]
        self._input = current_input
        self._pending = PushEvent()
        self._bass = 0
        self.calls: list[tuple] = []

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=True, mute=True,
                            input_select=True, power=True,
                            min_volume=0, max_volume=100)

    def is_connected(self) -> bool:
        return self._connected

    def connect(self) -> bool:
        self._connected = True
        return True

    def get_volume(self) -> int | None:
        return self._volume if self._connected else None

    def set_volume(self, level: int) -> bool:
        self.calls.append(("set_volume", level))
        if not self._connected:
            return False
        self._volume = level
        return True

    def volume_up(self) -> bool:
        self.calls.append(("volume_up",))
        if not self._connected:
            return False
        self._volume = min(100, self._volume + 1)
        return True

    def volume_down(self) -> bool:
        self.calls.append(("volume_down",))
        if not self._connected:
            return False
        self._volume = max(0, self._volume - 1)
        return True

    def get_mute(self) -> bool | None:
        return self._mute if self._connected else None

    def set_mute(self, muted: bool) -> bool:
        self.calls.append(("set_mute", muted))
        if not self._connected:
            return False
        self._mute = muted
        return True

    def get_power(self) -> bool | None:
        return self._power

    def set_power(self, on: bool) -> bool:
        self.calls.append(("set_power", on))
        self._power = on
        return True

    def get_inputs(self) -> list[str]:
        return list(self._inputs)

    def get_input(self) -> str | None:
        return self._input

    def set_input(self, name: str) -> bool:
        self.calls.append(("set_input", name))
        if name not in self._inputs:
            return False
        self._input = name
        return True

    # ---- extras -----------------------------------------------------
    def extras(self):
        return [
            ExtraSpec("bass", "number", "Bass", writable=True, min=-10, max=10, step=1, unit="dB"),
            ExtraSpec("model", "sensor", "Model"),
        ]

    def get_extra(self, key):
        if key == "bass":
            return self._bass
        if key == "model":
            return "FakeAmp"
        return None

    def set_extra(self, key, value):
        self.calls.append(("set_extra", key, value))
        if key == "bass":
            self._bass = int(value)
            return True
        return False

    # test helper: queue an unsolicited change
    def push(self, event: PushEvent) -> None:
        self._pending = event

    def poll_events(self) -> PushEvent:
        ev, self._pending = self._pending, PushEvent()
        return ev

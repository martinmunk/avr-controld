"""In-memory demo backend -- no hardware.

Useful for validating the MQTT/HA path end-to-end without a receiver, and as a
target for diagnostics. Behaves like a simple receiver with full capabilities.
"""

from __future__ import annotations

from .base import Backend, Capabilities, PushEvent


class DemoBackend(Backend):
    name = "Demo"

    def __init__(self, *, volume: int = 40, mute: bool = False, power: bool = True,
                 sources: list[str] | None = None, current_input: str | None = None):
        self._v = volume
        self._m = mute
        self._p = power
        self._sources = sources or ["TV", "CD", "Stream"]
        self._input = current_input or self._sources[0]

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=True, mute=True,
                            input_select=True, power=True, min_volume=0, max_volume=100)

    def is_connected(self) -> bool: return True
    def connect(self) -> bool: return True

    def get_volume(self): return self._v
    def set_volume(self, level): self._v = max(0, min(100, int(level))); return True
    def volume_up(self): self._v = min(100, self._v + 1); return True
    def volume_down(self): self._v = max(0, self._v - 1); return True

    def get_mute(self): return self._m
    def set_mute(self, muted): self._m = bool(muted); return True

    def get_power(self): return self._p
    def set_power(self, on): self._p = bool(on); return True

    def get_inputs(self): return list(self._sources)
    def get_input(self): return self._input
    def set_input(self, name):
        if name not in self._sources:
            return False
        self._input = name
        return True

    def poll_events(self) -> PushEvent:
        return PushEvent()

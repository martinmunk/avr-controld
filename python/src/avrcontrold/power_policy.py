"""Generic, backend-agnostic receiver power policy.

Not specific to any receiver (NAD/Denon/Opalum) -- it decides *whether the
receiver should be powered* from a set of named "demands", and the daemon
applies the decision to whatever backend it owns.

A demand is anything that wants the receiver on: AirPlay/shairport playing, the
TV/HDMI source being active (via CEC or Home Assistant), a manual override, etc.
The receiver should be ON while at least one demand is active. When the last
demand clears, an off-grace timer starts; if nothing reasserts a demand before
it expires, the receiver is powered OFF. A new demand powers it back ON and
cancels any pending off.

Pure and deterministic: time is passed in (monotonic seconds), no I/O, so it is
fully unit-testable. It emits a power command only when the desired state
changes, so the daemon issues minimal power commands (and never fights the user
powering the receiver by hand between transitions).
"""

from __future__ import annotations


class PowerPolicy:
    def __init__(self, off_grace_s: float = 120.0):
        self.off_grace_s = float(off_grace_s)
        self._demands: set[str] = set()
        self._commanded: bool | None = None      # last power we asked for
        self._off_deadline: float | None = None   # when to power off, if pending

    # ---- introspection (tests / logging) --------------------------------
    def demands(self) -> set[str]:
        return set(self._demands)

    def next_deadline(self) -> float | None:
        return self._off_deadline

    # ---- inputs ---------------------------------------------------------
    def set_demand(self, name: str, active: bool, now_s: float) -> bool | None:
        """Add/remove a named demand. Returns a power command (True/False) if the
        desired power state changed now, else None (e.g. an off was only scheduled)."""
        if active:
            self._demands.add(name)
        else:
            self._demands.discard(name)
        return self._evaluate(now_s)

    def tick(self, now_s: float) -> bool | None:
        """Flush a matured off-grace. Returns False when it powers off, else None."""
        if (self._off_deadline is not None and now_s >= self._off_deadline
                and not self._demands):
            self._off_deadline = None
            if self._commanded is not False:
                self._commanded = False
                return False
        return None

    # ---- core -----------------------------------------------------------
    def _evaluate(self, now_s: float) -> bool | None:
        if self._demands:
            # Something wants it on -> cancel any pending off, power on if needed.
            self._off_deadline = None
            if self._commanded is not True:
                self._commanded = True
                return True
            return None
        # No demands. Only schedule an off if WE powered it on -- never force a
        # receiver off that we didn't turn on (e.g. on at startup by hand).
        if self._commanded is True and self._off_deadline is None:
            self._off_deadline = now_s + self.off_grace_s
        return None

"""NAD backend speaking the T7xx RS232 protocol (``Variable=Value`` lines).

The `nad_receiver` library (what Home Assistant's own NAD integration uses)
supplies the serial transport; we do NOT let HA/nad_receiver own the port
directly (that would collide with this daemon). This backend is the single
owner and serves everyone through the socket protocol / MQTT bridge.

All reads go through :meth:`_q`, which checks that the reply names the variable
asked for. The NAD echoes every change made by remote or front panel as an
unsolicited ``Variable=Value`` line; one that lands mid-query would otherwise be
returned as that query's answer (seen live: ``Main.Monitor.Out?`` answered
``Main.Power=On``). Mismatched lines are kept and surfaced via
:meth:`poll_events`, together with anything drained from the serial input
buffer, so the daemon can see and log what the receiver did on its own.

Volume: the receiver works in dB (float). The socket protocol, ALSA plugins and
CEC relay all speak 0-100, so this backend maps dB<->percent over a configurable
range (default -80..0 dB). Relative up/down use the receiver's native +/- and so
are exact regardless of the map. The receiver object is injectable so the whole
backend is unit-testable with no serial port.
"""

from __future__ import annotations

import logging
import re
import time
from contextlib import nullcontext, suppress
from typing import Any, Callable, Optional, Protocol

from .base import Backend, Capabilities, ExtraSpec, PushEvent, Reconnect

# Extra NAD properties exposed beyond the core set. `static` ones are device
# configuration/identity: read at announce and after a write, not every poll
# (each query is a serial round-trip). `category` maps to HA's entity_category
# so config/diagnostic entities stay out of the main controls.
_NAD_EXTRAS = [
    ExtraSpec("bass", "number", "Bass", writable=True, min=-10, max=10, step=1,
              unit="dB", icon="mdi:music-clef-bass"),
    ExtraSpec("treble", "number", "Treble", writable=True, min=-10, max=10, step=1,
              unit="dB", icon="mdi:music-note"),
    ExtraSpec("tone_defeat", "switch", "Tone Defeat", writable=True, icon="mdi:tune-variant"),
    ExtraSpec("trim_center", "number", "Center Trim", writable=True, min=-12, max=12,
              step=1, unit="dB", icon="mdi:speaker"),
    ExtraSpec("trim_sub", "number", "Subwoofer Trim", writable=True, min=-12, max=12,
              step=1, unit="dB", icon="mdi:speaker"),
    ExtraSpec("trim_surround", "number", "Surround Trim", writable=True, min=-12, max=12,
              step=1, unit="dB", icon="mdi:speaker"),
    ExtraSpec("subwoofer", "switch", "Subwoofer", writable=True, icon="mdi:speaker",
              category="config"),
    ExtraSpec("zone2_power", "switch", "Zone 2 Power", writable=True, icon="mdi:power"),
    ExtraSpec("zone2_volume", "number", "Zone 2 Volume", writable=True, min=0, max=100,
              step=1, mode="slider", icon="mdi:volume-high"),
    ExtraSpec("zone2_mute", "switch", "Zone 2 Mute", writable=True, icon="mdi:volume-off"),
    ExtraSpec("sleep", "number", "Sleep Timer", writable=True, min=0, max=90, step=15,
              unit="min", icon="mdi:timer"),
    # The receiver's own idle timer (standby after ~30 min without signal/user
    # interaction). It silently fights the daemon's power policy -- the NAD went
    # off under a live TV three times before this was found -- so expose it and
    # keep it OFF when the daemon owns power (see cli.py).
    ExtraSpec("auto_standby", "switch", "Auto Standby", writable=True,
              icon="mdi:timer-off", category="config"),
    # Tuner.
    ExtraSpec("tuner_band", "select", "Tuner Band", writable=True,
              options=["FM", "AM", "DAB"], icon="mdi:radio"),
    ExtraSpec("fm_frequency", "number", "FM Frequency", writable=True, min=87.5,
              max=108.0, step=0.05, unit="MHz", icon="mdi:radio-fm"),
    ExtraSpec("am_frequency", "number", "AM Frequency", writable=True, min=531,
              max=1602, step=9, unit="kHz", icon="mdi:radio-am"),
    ExtraSpec("tuner_preset", "number", "Tuner Station Preset", writable=True, min=0,
              max=40, step=1, icon="mdi:radio"),
    ExtraSpec("fm_mute", "switch", "FM Mute", writable=True, icon="mdi:radio"),
    ExtraSpec("rds_name", "sensor", "RDS Name", icon="mdi:radio"),
    ExtraSpec("rds_text", "sensor", "RDS Text", icon="mdi:radio"),
    # Status / identity.
    ExtraSpec("listening_mode", "sensor", "Listening Mode", icon="mdi:surround-sound"),
    ExtraSpec("dimmer", "sensor", "Display Dimmer", icon="mdi:brightness-6"),
    ExtraSpec("video_mode", "sensor", "Video Mode", icon="mdi:television",
              category="diagnostic", static=True),
    ExtraSpec("model", "sensor", "Model", icon="mdi:information",
              category="diagnostic", static=True),
    ExtraSpec("version", "sensor", "Firmware", icon="mdi:information",
              category="diagnostic", static=True),
    ExtraSpec("serial", "sensor", "Serial Number", icon="mdi:information",
              category="diagnostic", static=True),
]
# key -> (NAD variable, type). Types: int | float | bool (On/Off) | yesno | str.
_NAD_EXTRA_VAR = {
    "bass": ("Main.Bass", "int"), "treble": ("Main.Treble", "int"),
    "tone_defeat": ("Main.ToneDefeat", "bool"),
    "trim_center": ("Main.Trim.Center", "int"), "trim_sub": ("Main.Trim.Sub", "int"),
    "trim_surround": ("Main.Trim.Surround", "int"),
    "subwoofer": ("Main.Speaker.Sub", "bool"),
    "zone2_power": ("Zone2.Power", "bool"),
    "zone2_mute": ("Zone2.Mute", "bool"), "sleep": ("Main.Sleep", "int"),
    "auto_standby": ("Main.AutoStandby", "bool"),
    "tuner_band": ("Tuner.Band", "str"), "fm_frequency": ("Tuner.FM.Frequency", "float"),
    "am_frequency": ("Tuner.AM.Frequency", "int"), "tuner_preset": ("Tuner.Preset", "int"),
    "fm_mute": ("Tuner.FM.Mute", "bool"), "rds_name": ("Tuner.FM.RDSName", "str"),
    "rds_text": ("Tuner.FM.RDSText", "str"),
    "listening_mode": ("Main.ListeningMode", "str"), "dimmer": ("Main.Dimmer", "str"),
    "video_mode": ("Main.VideoMode", "str"),
    "model": ("Main.Model", "str"), "version": ("Main.Version", "str"),
    "serial": ("Main.Serial", "str"),
}
# Per-source configuration (Source<n>.*), discovered per receiver. The audio
# input assignment is menu-only on the T757 (not on RS232), so it is not here.
_SOURCE_FIELDS = [
    ("enabled", "Enabled", "yesno", dict(component="switch", name="Enabled",
                                         icon="mdi:toggle-switch")),
    ("video_input", "VideoInput", "int", dict(component="number", name="Video Input",
                                              min=0, max=10, step=1, icon="mdi:video-input-hdmi")),
    ("analog_gain", "AnalogGain", "int", dict(component="number", name="Analog Gain",
                                              min=-12, max=12, step=1, unit="dB",
                                              icon="mdi:amplifier")),
    ("preset", "Preset", "int", dict(component="number", name="Preset", min=0, max=5,
                                     step=1, icon="mdi:tune")),
]
_MAX_PRESETS = 5

_VOL_RE = re.compile(r"-?\d+(?:\.\d+)?")

log = logging.getLogger(__name__)


class NadClient(Protocol):
    """The subset of nad_receiver.NADReceiver this backend uses: its transport."""
    transport: Any


def _open_nad(device: str) -> NadClient:
    from nad_receiver import NADReceiver  # lazy: only needed live
    return NADReceiver(device)


def _on(value: Optional[str]) -> Optional[bool]:
    if value is None:
        return None
    return str(value).strip().lower() == "on"


class NADBackend(Backend):
    name = "NAD Serial"

    def __init__(self, device: str, *, receiver: NadClient | None = None,
                 min_db: float = -80.0, max_db: float = 0.0,
                 sources: list[str] | None = None,
                 factory: Callable[[str], NadClient] | None = None,
                 reconnect_interval: float = 5.0,
                 clock: Callable[[], float] = time.monotonic):
        if max_db <= min_db:
            raise ValueError("max_db must be greater than min_db")
        self._device = device
        self._rx = receiver
        self._factory = factory or _open_nad      # device path -> client
        self._reconnect = Reconnect(reconnect_interval, clock)
        self._in_connect = False
        self._min_db = float(min_db)
        self._max_db = float(max_db)
        self._sources = list(sources or [])       # manual override (names)
        self._num_to_name: dict[int, str] = {}     # discovered Source<n>.Name
        self._name_to_num: dict[str, int] = {}
        self._enabled_nums: list[int] = []
        self._presets: dict[int, str] = {}         # discovered Preset<n>.Name
        self._dyn_specs: list[ExtraSpec] = []      # per-source/preset extras
        self._dyn_vars: dict[str, tuple[str, str]] = {}
        self._pending: list[tuple[str, str]] = []  # unsolicited lines caught mid-query
        self._connected = receiver is not None

    # ---- dB <-> percent -------------------------------------------------
    def _db_to_pct(self, db: float) -> int:
        frac = (db - self._min_db) / (self._max_db - self._min_db)
        return max(0, min(100, round(frac * 100)))

    def _pct_to_db(self, pct: int) -> float:
        pct = max(0, min(100, pct))
        return self._min_db + (pct / 100.0) * (self._max_db - self._min_db)

    # ---- connection -----------------------------------------------------
    def _drop_rx(self) -> None:
        """Close and forget the client so the next connect() reopens the
        configured path (which may now resolve to a different ttyUSBn)."""
        rx, self._rx = self._rx, None
        if rx is None:
            return
        ser = getattr(getattr(rx, "transport", None), "ser", None)
        with suppress(Exception):
            if ser is not None:
                ser.close()

    def connect(self) -> bool:
        """Open (or reopen) the port and verify the receiver answers.

        Rate-limited: once the link has failed, attempts are spaced
        ``reconnect_interval`` apart and return False in between without
        touching the port, so callers can call this freely.
        """
        if self._in_connect:
            return self._connected
        if self._rx is not None and not self._connected:
            self._drop_rx()                   # stale descriptor (EIO forever)
        if self._rx is None:
            if not self._reconnect.due():
                return False
            try:
                self._rx = self._factory(self._device)
            except Exception as exc:
                log.log(self._reconnect.failed(),
                        "NAD: cannot open %s: %s", self._device, exc)
                self._connected = False
                return False
        self._in_connect = True
        try:
            # A query doubles as a liveness check.
            self._connected = self._q("Main.Power") is not None
        finally:
            self._in_connect = False
        if not self._connected:
            # Opened but no (valid) reply: count it like an I/O error so the
            # next attempt waits for the interval instead of every query.
            log.log(self._reconnect.failed(), "NAD: no reply on %s", self._device)
            self._drop_rx()
            return False
        if self._reconnect.succeeded():
            log.info("NAD: reconnected on %s", self._device)
        if not self._num_to_name:
            self._discover()
        return True

    def close(self) -> None:
        self._drop_rx()
        self._connected = False

    def _discover(self) -> None:
        """Read Source<n>.Name/Enabled so HA gets a source selector with the NAD's
        real names and set_input can map a name back to its number; build the
        per-source config extras; find the listening-preset names."""
        for n in range(1, 11):
            name = self._q(f"Source{n}.Name")
            if not name:
                continue
            self._num_to_name[n] = name
            self._name_to_num[name] = n
            en = self._q(f"Source{n}.Enabled")
            if en is None or en.strip().lower() == "yes":
                self._enabled_nums.append(n)
            for suffix, var, typ, kw in _SOURCE_FIELDS:
                key = f"source{n}_{suffix}"
                kw = dict(kw)
                # "TV source: Enabled" -- distinct from receiver-wide names such
                # as "Tuner Station Preset" when a source is itself called Tuner.
                kw["name"] = f"{name} source: {kw['name']}"
                self._dyn_specs.append(ExtraSpec(key, writable=True, category="config",
                                                 static=True, **kw))
                self._dyn_vars[key] = (f"Source{n}.{var}", typ)
        for n in range(1, _MAX_PRESETS + 1):
            pname = self._q(f"Preset{n}.Name")
            if not pname:
                continue
            self._presets[n] = pname
            key = f"preset{n}_name"
            self._dyn_specs.append(ExtraSpec(key, "sensor", f"Preset {n} Name",
                                             icon="mdi:tune", category="diagnostic",
                                             static=True))
            self._dyn_vars[key] = (f"Preset{n}.Name", "str")
        if self._num_to_name:
            log.info("NAD: discovered %d sources (%d enabled), %d presets",
                     len(self._num_to_name), len(self._enabled_nums), len(self._presets))

    def is_connected(self) -> bool:
        return self._connected

    def _guard(self):
        """The client to talk to, or None while the link is down (no I/O is
        attempted then, except the rate-limited reconnect)."""
        if self._in_connect:
            return self._rx
        if self._rx is None or not self._connected:
            self.connect()
        return self._rx if self._connected else None

    def capabilities(self) -> Capabilities:
        return Capabilities(absolute=True, query=True, mute=True,
                            input_select=bool(self._sources or self._num_to_name),
                            power=True, standby_timeout=True,
                            min_volume=0, max_volume=100)

    # ---- protocol primitives -------------------------------------------
    def _parse(self, line: str | None) -> tuple[str, str] | None:
        if not line or "=" not in line:
            return None
        name, val = line.split("=", 1)
        return name.strip(), val.strip()

    def _cmd(self, command: str, var: str) -> str | None:
        """Send `command` and return the value of the reply, which must name
        `var`. A reply naming another variable is an unsolicited echo that
        landed mid-query: it is kept for poll_events and this read is void."""
        parsed = self._parse(self.raw(command))
        if parsed is None:
            return None
        name, val = parsed
        if name != var:
            log.debug("NAD: reply %s=%s does not match query %s (kept as event)",
                      name, val, var)
            self._pending.append((name, val))
            return None
        return val

    def _q(self, var: str) -> str | None:
        return self._cmd(var + "?", var)

    def _s(self, var: str, value: str) -> bool:
        return self._cmd(f"{var}={value}", var) is not None

    def _vol_db(self, s: str | None) -> float | None:
        # Some models append units ("dB") to the value; take the number only.
        m = _VOL_RE.match(s.strip()) if s else None
        try:
            return float(m.group()) if m else None
        except ValueError:
            return None

    # ---- volume ---------------------------------------------------------
    def get_volume(self) -> int | None:
        db = self._vol_db(self._q("Main.Volume"))
        return None if db is None else self._db_to_pct(db)

    def set_volume(self, level: int) -> bool:
        db = self._pct_to_db(level)
        return self._s("Main.Volume", f"{db:.0f}")

    def volume_up(self) -> bool:
        return self._cmd("Main.Volume+", "Main.Volume") is not None

    def volume_down(self) -> bool:
        return self._cmd("Main.Volume-", "Main.Volume") is not None

    # ---- mute -----------------------------------------------------------
    def get_mute(self) -> bool | None:
        return _on(self._q("Main.Mute"))

    def set_mute(self, muted: bool) -> bool:
        return self._s("Main.Mute", "On" if muted else "Off")

    # ---- power ----------------------------------------------------------
    def get_power(self) -> bool | None:
        return _on(self._q("Main.Power"))

    def set_power(self, on: bool) -> bool:
        return self._s("Main.Power", "On" if on else "Off")

    # ---- input / source -------------------------------------------------
    def get_inputs(self) -> list[str]:
        if self._sources:
            return list(self._sources)
        return [self._num_to_name[n] for n in self._enabled_nums]

    def _source_name(self, v: str | None) -> str | None:
        if v is None:
            return None
        try:
            return self._num_to_name.get(int(v), str(v))
        except (ValueError, TypeError):
            return str(v)

    def get_input(self) -> str | None:
        return self._source_name(self._q("Main.Source"))

    def set_input(self, name: str) -> bool:
        # Accept a discovered source name, or a raw number/string.
        target = name
        if name in self._name_to_num:
            target = str(self._name_to_num[name])
        return self._s("Main.Source", target)

    # ---- extra properties ----------------------------------------------
    def extras(self) -> list[ExtraSpec]:
        ex = list(_NAD_EXTRAS)
        names = self.get_inputs()
        if names:
            ex.append(ExtraSpec("zone2_source", "select", "Zone 2 Source",
                                writable=True, options=names, icon="mdi:import"))
        return ex + list(self._dyn_specs)

    def _spec(self, key: str) -> tuple[str, str] | None:
        return _NAD_EXTRA_VAR.get(key) or self._dyn_vars.get(key)

    def get_extra(self, key: str):
        if key == "zone2_volume":
            db = self._vol_db(self._q("Zone2.Volume"))
            return None if db is None else self._db_to_pct(db)   # same map as Main
        if key == "zone2_source":
            return self._source_name(self._q("Zone2.Source"))
        spec = self._spec(key)
        if spec is None:
            return None
        var, typ = spec
        v = self._q(var)
        if v is None:
            return None
        if typ == "int":
            try:
                return int(float(v))
            except ValueError:
                return None
        if typ == "float":
            try:
                return float(v)
            except ValueError:
                return None
        if typ == "bool":
            return v.strip().lower() == "on"
        if typ == "yesno":
            return v.strip().lower() == "yes"
        return v

    def set_extra(self, key: str, value) -> bool:
        if key == "zone2_volume":
            db = self._pct_to_db(int(value))
            return self._s("Zone2.Volume", f"{db:.0f}")
        if key == "zone2_source":
            num = self._name_to_num.get(str(value))
            return self._s("Zone2.Source", str(num) if num is not None else str(value))
        spec = self._spec(key)
        if spec is None:
            return False
        var, typ = spec
        truthy = value in (True, 1, "1", "on", "ON", "true", "True", "yes", "Yes")
        if typ == "bool":
            val = "On" if truthy else "Off"
        elif typ == "yesno":
            val = "Yes" if truthy else "No"
        elif typ == "int":
            val = str(int(value))
        elif typ == "float":
            val = f"{float(value):.2f}"
        else:
            val = str(value)
        return self._s(var, val)

    # ---- unsolicited echoes (remote / front panel / the NAD's own timers) --
    def _drain_serial(self) -> list[tuple[str, str]]:
        """Read whatever the receiver pushed since the last command. The
        transport flushes its input buffer before every query, so this is the
        only place those lines survive; call it before the state sweep."""
        tr = getattr(self._rx, "transport", None)
        ser = getattr(tr, "ser", None)
        if ser is None:
            return []
        out: list[tuple[str, str]] = []
        try:
            with getattr(tr, "lock", None) or nullcontext():
                while ser.in_waiting:
                    raw = ser.read_until(b"\r")
                    parsed = self._parse(raw.decode(errors="replace").strip())
                    if parsed:
                        out.append(parsed)
        except Exception as exc:
            log.debug("NAD: drain failed: %s", exc)
        return out

    def poll_events(self) -> PushEvent:
        lines, self._pending = self._pending, []
        lines += self._drain_serial()
        ev = PushEvent()
        for name, val in lines:
            if name == "Main.Power":
                ev.power = _on(val)
            elif name == "Main.Mute":
                ev.mute = _on(val)
            elif name == "Main.Volume":
                db = self._vol_db(val)
                if db is not None:
                    ev.volume = self._db_to_pct(db)
            elif name == "Main.Source":
                ev.input = self._source_name(val)
            else:
                ev.other[name] = val
        return ev

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
            log.log(self._reconnect.failed(),
                    "NAD: raw '%s' failed: %s (will reopen %s)",
                    command, exc, self._device)
            self._connected = False
            return None

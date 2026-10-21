"""Offline tests for NADBackend using a fake nad_receiver (no serial port)."""

import pytest

from avrcontrold.backends.nad import NADBackend


class _FakeSerial:
    """Minimal pyserial stand-in: queued unsolicited lines."""
    def __init__(self):
        self.buf = b""
    def push(self, line: str):
        self.buf += line.encode() + b"\r"
    @property
    def in_waiting(self):
        return len(self.buf)
    def read_until(self, term=b"\r"):
        i = self.buf.index(term) + 1
        out, self.buf = self.buf[:i], self.buf[i:]
        return out


class _FakeTransport:
    """Serves Variable?/Variable=Value over the fake's state, like the NAD."""
    _CORE = {"Main.Power": "power", "Main.Mute": "mute", "Main.Source": "source"}

    def __init__(self, nad):
        self.nad = nad
        self.ser = _FakeSerial()
        self.mismatch_once = None   # reply with this line instead, once
        self.sent = []

    def communicate(self, cmd):
        self.nad._maybe_raise()
        self.sent.append(cmd)
        self.ser.buf = b""          # like SerialPortTransport.reset_input_buffer
        if self.mismatch_once:
            r, self.mismatch_once = self.mismatch_once, None
            return r
        if cmd.endswith("?"):
            var = cmd[:-1]
            if var == "Main.Volume":
                return f"Main.Volume={self.nad.db:.0f}"
            if var in self._CORE:
                return f"{var}={getattr(self.nad, self._CORE[var])}"
            return f"{var}={self.nad.vars.get(var, '')}" if var in self.nad.vars else ""
        if cmd in ("Main.Volume+", "Main.Volume-"):
            self.nad.db += 1 if cmd[-1] == "+" else -1
            return f"Main.Volume={self.nad.db:.0f}"
        if "=" in cmd:
            var, val = cmd.split("=", 1)
            if var == "Main.Volume":
                self.nad.db = float(val)
            elif var in self._CORE:
                setattr(self.nad, self._CORE[var], val)
            else:
                self.nad.vars[var] = val
            return cmd
        return ""


class FakeNad:
    """Mimics nad_receiver.NADReceiver operator semantics in memory."""
    def __init__(self, db=-40.0, power="On", mute="Off", source="1"):
        self.db = db
        self.power = power
        self.mute = mute
        self.source = source
        self.raise_next = False
        self.vars = {"Main.Bass": "-2", "Main.Treble": "0", "Main.Model": "T757",
                     "Zone2.Power": "Off", "Zone2.Volume": "-20", "Main.Sleep": "0",
                     "Main.ListeningMode": "None", "Main.ToneDefeat": "Off",
                     "Main.Trim.Sub": "0", "Tuner.FM.Frequency": "87.50",
                     "Source1.Enabled": "Yes", "Source1.AnalogGain": "0"}
        self.transport = _FakeTransport(self)

    def _maybe_raise(self):
        if self.raise_next:
            self.raise_next = False
            raise IOError("serial boom")

    def main_volume(self, operator, value=None):
        self._maybe_raise()
        if operator == "?":
            return self.db
        if operator == "=":
            self.db = float(value)
        elif operator == "+":
            self.db += 1
        elif operator == "-":
            self.db -= 1
        return self.db

    def main_power(self, operator, value=None):
        self._maybe_raise()
        if operator == "?":
            return self.power
        if operator == "=":
            self.power = value
        return self.power

    def main_mute(self, operator, value=None):
        self._maybe_raise()
        if operator == "?":
            return self.mute
        if operator == "=":
            self.mute = value
        return self.mute

    def main_source(self, operator, value=None):
        self._maybe_raise()
        if operator == "?":
            return self.source
        if operator == "=":
            self.source = value
        return self.source


def make(**kw):
    rx = FakeNad(**kw)
    be = NADBackend("/dev/ttyUSB0", receiver=rx, sources=["Source 1", "Source 2"])
    return rx, be


def test_capabilities():
    _, be = make()
    caps = be.capabilities()
    assert caps.absolute and caps.query and caps.mute and caps.power
    assert caps.input_select                # sources configured
    assert (caps.min_volume, caps.max_volume) == (0, 100)


def test_db_percent_mapping_default_range():
    _, be = make(db=-40.0)      # midpoint of -80..0
    assert be.get_volume() == 50
    rx, be = make(db=0.0)
    assert be.get_volume() == 100
    rx, be = make(db=-80.0)
    assert be.get_volume() == 0


def test_set_volume_maps_percent_to_db():
    rx, be = make()
    assert be.set_volume(80)
    assert rx.db == -16.0        # -80 + 0.8*80
    assert be.get_volume() == 80


def test_relative_volume_uses_native_steps():
    rx, be = make(db=-40.0)
    assert be.volume_up()
    assert rx.db == -39.0
    assert be.volume_down()
    assert rx.db == -40.0


def test_mute_roundtrip():
    rx, be = make(mute="Off")
    assert be.get_mute() is False
    assert be.set_mute(True)
    assert rx.mute == "On"
    assert be.get_mute() is True


def test_power_roundtrip():
    rx, be = make(power="On")
    assert be.get_power() is True
    assert be.set_power(False)
    assert rx.power == "Off"
    assert be.get_power() is False


def test_source_roundtrip():
    rx, be = make(source="Source 1")
    assert be.get_input() == "Source 1"
    assert be.set_input("Source 2")
    assert rx.source == "Source 2"
    assert be.get_inputs() == ["Source 1", "Source 2"]


def test_input_select_capability_off_without_sources():
    rx = FakeNad()
    be = NADBackend("/dev/ttyUSB0", receiver=rx)      # no sources
    assert be.capabilities().input_select is False


def test_custom_db_range():
    rx = FakeNad(db=-50.0)
    be = NADBackend("/dev/ttyUSB0", receiver=rx, min_db=-100.0, max_db=0.0)
    assert be.get_volume() == 50


def test_bad_db_range_rejected():
    with pytest.raises(ValueError):
        NADBackend("/dev/ttyUSB0", receiver=FakeNad(), min_db=0.0, max_db=-80.0)


def test_io_error_marks_disconnected_and_unknown():
    rx, be = make()
    rx.raise_next = True
    assert be.get_volume() is None
    assert be.is_connected() is False


class Clock:
    def __init__(self): self.t = 0.0
    def __call__(self): return self.t


def test_dead_port_is_dropped_and_reopened_after_the_interval():
    """A USB hub reset re-enumerates the adapter: the old descriptor returns
    EIO forever. The backend must close it and open the configured path again,
    but only every reconnect_interval, and stay quiet in between."""
    clock = Clock()
    old, new = FakeNad(db=-40.0), FakeNad(db=-30.0)
    opened = []
    def factory(dev):
        opened.append(dev)
        return new
    be = NADBackend("/dev/serial/by-id/x", receiver=old, factory=factory,
                    reconnect_interval=5.0, clock=clock)
    assert be.get_volume() == 50
    old.raise_next = True
    assert be.get_volume() is None and not be.is_connected()
    # Within the interval: no reopen, no traffic on the dead client.
    old.raise_next = True                       # would raise if touched
    assert be.get_volume() is None
    assert be.get_power() is None
    assert opened == [] and old.raise_next is True
    # After the interval: reopened on the configured path, state back.
    clock.t = 5.0
    assert be.get_volume() == 62                # from the new client (-30 dB)
    assert be.is_connected()
    assert opened == ["/dev/serial/by-id/x"]


def test_reopen_failures_are_rate_limited():
    clock = Clock()
    attempts = []
    def factory(dev):
        attempts.append(clock.t)
        raise OSError(2, "No such file or directory")
    rx = FakeNad()
    be = NADBackend("/dev/ttyUSB0", receiver=rx, factory=factory,
                    reconnect_interval=5.0, clock=clock)
    rx.raise_next = True
    assert be.get_volume() is None
    for t in (1.0, 4.9):
        clock.t = t
        assert be.connect() is False and be.get_power() is None
    assert attempts == []
    for t in (5.0, 7.0, 10.0, 11.0, 15.0):
        clock.t = t
        be.connect(); be.get_power()
    assert attempts == [5.0, 10.0, 15.0]
    assert not be.is_connected()


def test_silent_port_counts_as_a_failure():
    """Opened but nothing answers (cable to the receiver unplugged): the
    liveness query fails without an exception; that must still be rate-limited."""
    class _SilentTransport:
        def communicate(self, command): return ""
    class Silent:
        transport = _SilentTransport()
    clock = Clock()
    opened = []
    def factory(dev):
        opened.append(clock.t)
        return Silent()
    be = NADBackend("/dev/ttyUSB0", factory=factory, reconnect_interval=5.0,
                    clock=clock)
    assert be.connect() is False and not be.is_connected()
    for t in (1.0, 2.0, 4.9):
        clock.t = t
        assert be.get_power() is None
    clock.t = 5.0
    assert be.get_power() is None
    assert opened == [0.0, 5.0]


def test_close_releases_the_client():
    rx, be = make()
    be.close()
    assert not be.is_connected()


# ---- extras (via raw/transport) --------------------------------------------
def test_extras_list_and_read():
    _, be = make()
    keys = {e.key for e in be.extras()}
    assert {"bass", "treble", "zone2_power", "zone2_volume", "model"} <= keys
    assert be.get_extra("bass") == -2          # int
    assert be.get_extra("model") == "T757"     # str
    assert be.get_extra("zone2_power") is False  # "Off" -> bool


def test_extras_write():
    rx, be = make()
    assert be.set_extra("bass", 4)
    assert rx.vars["Main.Bass"] == "4"
    assert be.get_extra("bass") == 4
    assert be.set_extra("zone2_power", True)
    assert rx.vars["Zone2.Power"] == "On"
    assert be.get_extra("zone2_power") is True


def test_auto_standby_extra():
    # The receiver's own idle timer: exposed as a switch so it can be seen and
    # disabled (it silently powers the NAD off under a live TV otherwise).
    rx, be = make()
    rx.vars["Main.AutoStandby"] = "On"
    assert "auto_standby" in {e.key for e in be.extras()}
    assert be.get_extra("auto_standby") is True
    assert be.set_extra("auto_standby", False)
    assert rx.vars["Main.AutoStandby"] == "Off"
    assert be.get_extra("auto_standby") is False


def test_raw_passthrough():
    _, be = make()
    assert be.raw("Main.Model?") == "Main.Model=T757"
    assert be.raw("Main.Bass=3") == "Main.Bass=3"


def test_source_discovery_maps_names_and_numbers():
    rx = FakeNad(source="1")
    rx.vars.update({"Source1.Name": "TV", "Source1.Enabled": "Yes",
                    "Source2.Name": "AirPlay", "Source2.Enabled": "No",
                    "Source3.Name": "CD", "Source3.Enabled": "Yes"})
    be = NADBackend("/dev/ttyUSB0", receiver=rx)   # no manual sources -> discover
    assert be.connect()
    assert be.capabilities().input_select is True
    assert be.get_inputs() == ["TV", "CD"]          # only enabled
    assert be.get_input() == "TV"                    # current source 1 -> name
    assert be.set_input("CD")                        # name -> number 3
    assert rx.source == "3"
    assert be.get_input() == "CD"


def test_zone2_source_select_extra():
    rx = FakeNad(source="1")
    rx.vars.update({"Source1.Name": "TV", "Source1.Enabled": "Yes",
                    "Source2.Name": "AirPlay", "Source2.Enabled": "Yes",
                    "Zone2.Source": "1"})
    be = NADBackend("/dev/ttyUSB0", receiver=rx)
    assert be.connect()
    specs = {e.key: e for e in be.extras()}
    assert "zone2_source" in specs
    assert specs["zone2_source"].component == "select"
    assert specs["zone2_source"].options == ["TV", "AirPlay"]
    assert be.get_extra("zone2_source") == "TV"          # Zone2.Source=1 -> TV
    assert be.set_extra("zone2_source", "AirPlay")       # name -> number 2
    assert rx.vars["Zone2.Source"] == "2"
    assert be.get_extra("zone2_source") == "AirPlay"


def test_zone2_volume_is_percent_like_main():
    rx = FakeNad()                       # Zone2.Volume default "-20"
    be = NADBackend("/dev/ttyUSB0", receiver=rx, sources=["Source 1"])
    specs = {e.key: e for e in be.extras()}
    z = specs["zone2_volume"]
    assert z.component == "number" and z.min == 0 and z.max == 100 and z.mode == "slider"
    assert be.get_extra("zone2_volume") == 75          # -20 dB over -80..0 -> 75%
    assert be.set_extra("zone2_volume", 50)            # 50% -> -40 dB
    assert rx.vars["Zone2.Volume"] == "-40"
    assert be.get_extra("zone2_volume") == 50


# ---- the unsolicited-echo problem -----------------------------------------
def test_mismatched_reply_is_not_taken_as_answer_but_kept_as_event():
    # Live case: the user pressed power mid-query; "Main.Monitor.Out?" was
    # answered "Main.Power=On". The read must be void and the echo surfaced.
    rx, be = make(power="Off")
    rx.transport.mismatch_once = "Main.Power=On"
    assert be.get_volume() is None                 # void, not "On" parsed as volume
    ev = be.poll_events()
    assert ev.power is True
    assert be.poll_events().empty()                # consumed


def test_drains_unsolicited_lines_from_serial():
    rx, be = make()
    rx.transport.ser.push("Main.Power=Off")
    rx.transport.ser.push("Main.Volume=-20")
    rx.transport.ser.push("Zone2.Power=On")
    ev = be.poll_events()
    assert ev.power is False
    assert ev.volume == 75                          # -20 dB over -80..0
    assert ev.other == {"Zone2.Power": "On"}
    assert "power=OFF" in ev.describe() and "Zone2.Power=On" in ev.describe()
    assert be.poll_events().empty()


def test_core_reads_via_name_checked_query():
    rx, be = make(power="On", mute="Off", db=-40.0)
    assert be.get_power() is True
    assert be.get_mute() is False
    assert be.get_volume() == 50
    assert be.volume_up() and be.get_volume() == 51
    assert be.set_power(False) and rx.power == "Off"


def test_new_extras_types_and_categories():
    rx, be = make()
    specs = {e.key: e for e in be.extras()}
    assert specs["tone_defeat"].component == "switch"
    assert specs["serial"].static and specs["serial"].category == "diagnostic"
    assert specs["auto_standby"].category == "config"
    assert be.get_extra("tone_defeat") is False
    assert be.set_extra("tone_defeat", True) and rx.vars["Main.ToneDefeat"] == "On"
    assert be.get_extra("fm_frequency") == 87.5                      # float
    assert be.set_extra("fm_frequency", 101.3)
    assert rx.vars["Tuner.FM.Frequency"] == "101.30"
    assert be.set_extra("trim_sub", -3) and be.get_extra("trim_sub") == -3


def test_per_source_config_extras_discovered():
    rx = FakeNad()
    rx.vars.update({"Source1.Name": "TV", "Source2.Name": "AirPlay",
                    "Source2.Enabled": "No", "Preset1.Name": "Movie"})
    be = NADBackend("/dev/ttyUSB0", receiver=rx)
    assert be.connect()
    assert be.get_inputs() == ["TV"]                # Source2 disabled
    keys = {e.key for e in be.extras()}
    assert {"source1_enabled", "source1_analog_gain", "source2_enabled",
            "preset1_name"} <= keys
    spec = {e.key: e for e in be.extras()}["source2_enabled"]
    assert spec.name == "AirPlay source: Enabled" and spec.static and spec.category == "config"
    assert be.get_extra("source2_enabled") is False                  # "No"
    assert be.set_extra("source2_enabled", True) and rx.vars["Source2.Enabled"] == "Yes"
    assert be.get_extra("preset1_name") == "Movie"

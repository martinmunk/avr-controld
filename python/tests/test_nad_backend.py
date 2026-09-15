"""Offline tests for NADBackend using a fake nad_receiver (no serial port)."""

import pytest

from avrcontrold.backends.nad import NADBackend


class FakeNad:
    """Mimics nad_receiver.NADReceiver operator semantics in memory."""
    def __init__(self, db=-40.0, power="On", mute="Off", source="Source 1"):
        self.db = db
        self.power = power
        self.mute = mute
        self.source = source
        self.raise_next = False

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

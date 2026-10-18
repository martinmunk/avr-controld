"""The Backend ABC is implementable and behaves per contract (offline)."""

from avrcontrold.backends.base import PushEvent
from fakes import FakeBackend


def test_capabilities_and_state():
    be = FakeBackend(volume=42, mute=False, power=True)
    caps = be.capabilities()
    assert caps.absolute and caps.query and caps.mute
    assert be.is_connected()
    assert be.get_volume() == 42
    assert be.get_mute() is False
    assert be.get_power() is True


def test_set_and_readback():
    be = FakeBackend(volume=20)
    assert be.set_volume(35)
    assert be.get_volume() == 35
    assert be.set_mute(True)
    assert be.get_mute() is True


def test_relative_volume():
    be = FakeBackend(volume=50)
    be.volume_up(); be.volume_up()
    assert be.get_volume() == 52
    be.volume_down()
    assert be.get_volume() == 51


def test_input_selection():
    be = FakeBackend(inputs=["TV", "CD"], current_input="TV")
    assert be.get_inputs() == ["TV", "CD"]
    assert be.set_input("CD")
    assert be.get_input() == "CD"
    assert not be.set_input("Nonexistent")


def test_unknown_state_when_disconnected():
    be = FakeBackend(connected=False)
    assert be.get_volume() is None
    assert be.get_mute() is None
    assert not be.set_volume(30)


def test_push_events():
    be = FakeBackend()
    assert be.poll_events().empty()
    be.push(PushEvent(volume=33, mute=True))
    ev = be.poll_events()
    assert ev.volume == 33 and ev.mute is True
    assert be.poll_events().empty()   # consumed

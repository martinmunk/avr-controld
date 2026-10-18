"""The power policy must own the receiver's power outright: the receiver's own
auto-standby timer is turned off at startup (unless auto_standby = keep)."""

from avrcontrold.backends.base import ExtraSpec
from avrcontrold.cli import own_receiver_power

from fakes import FakeBackend


class AutoStandbyBackend(FakeBackend):
    def __init__(self, state=True):
        super().__init__()
        self.auto_standby = state

    def extras(self):
        return super().extras() + [
            ExtraSpec("auto_standby", "switch", "Auto Standby", writable=True)]

    def get_extra(self, key):
        if key == "auto_standby":
            return self.auto_standby
        return super().get_extra(key)

    def set_extra(self, key, value):
        if key == "auto_standby":
            self.calls.append(("set_extra", key, value))
            self.auto_standby = bool(value)
            return True
        return super().set_extra(key, value)


def test_turns_auto_standby_off():
    be = AutoStandbyBackend(True)
    assert own_receiver_power(be, {}) is True
    assert be.auto_standby is False
    assert ("set_extra", "auto_standby", False) in be.calls


def test_already_off_is_left_alone():
    be = AutoStandbyBackend(False)
    assert own_receiver_power(be, {}) is False
    assert not any(c[0] == "set_extra" for c in be.calls)


def test_keep_leaves_it():
    be = AutoStandbyBackend(True)
    assert own_receiver_power(be, {"auto_standby": "keep"}) is False
    assert be.auto_standby is True


def test_backend_without_setting():
    be = FakeBackend()
    assert own_receiver_power(be, {}) is None
    assert not any(c[0] == "set_extra" for c in be.calls)

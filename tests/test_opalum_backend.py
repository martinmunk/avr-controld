"""Offline tests for OpalumBackend: a fake client (always), and the real
opalum_receiver library over an injected transport when it is importable."""

import pytest

from avrcontrold.backends.opalum import OpalumBackend

SOURCES = ["Toslink1", "Toslink2", "Coaxial", "Bluetooth", "Analog", "Mixer"]


class FakeOpalum:
    """Cached-state client, like the library: None until set."""
    def __init__(self):
        self.power = self.volume = self.muted = self.source = self.sta = None
        self.calls = []
        self.fail_next = False
        self.raise_next = False

    def _ok(self, name):
        self.calls.append(name)
        if self.raise_next:
            self.raise_next = False
            raise IOError("serial boom")
        if self.fail_next:
            self.fail_next = False
            return False
        return True

    def set_power(self, on):
        if self._ok(f"pwr{int(on)}"): self.power = on; return True
        return False
    def get_power(self): return self.power
    def set_volume(self, level):
        if self._ok(f"vol{level}"): self.volume = level; return True
        return False
    def volume_up(self): return self._ok("vol+")
    def volume_down(self): return self._ok("vol-")
    def get_volume(self): return self.volume
    def set_mute(self, muted):
        if self._ok("vol00" if muted else "unmute"): self.muted = muted; return True
        return False
    def get_mute(self): return self.muted
    def set_source(self, source):
        if source not in SOURCES: raise ValueError(source)
        if self._ok(f"src{SOURCES.index(source)}"): self.source = source; return True
        return False
    def get_source(self): return self.source
    def available_sources(self): return list(SOURCES)
    def set_standby_timeout(self, enabled):
        if self._ok(f"sta{int(enabled)}"): self.sta = enabled; return True
        return False
    def get_standby_timeout(self): return self.sta


def make():
    rx = FakeOpalum()
    return rx, OpalumBackend("/dev/ttyUSB0", receiver=rx)


def test_capabilities_write_only_protocol():
    _, be = make()
    c = be.capabilities()
    assert c.absolute and c.mute and c.power and c.input_select and c.standby_timeout
    assert not c.query                       # nothing can be read back
    assert be.is_connected()


def test_state_unknown_until_set():
    rx, be = make()
    assert be.get_power() is None and be.get_volume() is None
    assert be.set_power(True) and be.get_power() is True
    assert be.set_volume(42) and be.get_volume() == 42
    assert be.set_mute(True) and be.get_mute() is True
    assert rx.calls == ["pwr1", "vol42", "vol00"]


def test_volume_clamped_to_0_100():
    rx, be = make()
    assert be.set_volume(250) and rx.volume == 100
    assert be.set_volume(-5) and rx.volume == 0


def test_sources():
    rx, be = make()
    assert be.get_inputs() == SOURCES
    assert be.set_input("Mixer") and be.get_input() == "Mixer"
    assert rx.calls[-1] == "src5"
    assert be.set_input("Nope") is False     # unknown source -> no command


def test_auto_standby_extra():
    rx, be = make()
    assert [e.key for e in be.extras()] == ["auto_standby"]
    assert be.get_extra("auto_standby") is None
    assert be.set_extra("auto_standby", False) and rx.sta is False
    assert rx.calls[-1] == "sta0"
    assert be.get_extra("auto_standby") is False


def test_nack_and_io_error():
    rx, be = make()
    rx.fail_next = True
    assert be.set_power(True) is False        # not acknowledged
    assert be.is_connected()
    rx.raise_next = True
    assert be.set_volume(10) is False
    assert not be.is_connected()
    assert be.set_volume(10) and be.is_connected()   # next call reconnects


# ---- the real library over a fake serial line -------------------------------
try:
    import opalum_receiver
except ImportError:            # offline suite: the library is a runtime extra
    opalum_receiver = None


@pytest.mark.skipif(opalum_receiver is None, reason="opalum_receiver not installed")
def test_real_library_wire_format():
    class _EchoTransport(opalum_receiver.transport.BaseTransport):
        def __init__(self):
            self.sent = []
        def command(self, request):
            self.sent.append(request)
            return f"{request}\r\nOK\r\n"
        def close(self):
            pass

    tr = _EchoTransport()
    rx = opalum_receiver.OpalumReceiver(transport=tr, power_on=False)
    be = OpalumBackend("/dev/null", receiver=rx)
    assert be.set_power(True) and be.set_input("Mixer") and be.set_volume(50)
    assert be.set_extra("auto_standby", False)
    assert tr.sent == ["pwr1", "src5", "vol31", "sta0"]   # 50% -> 0x63*50//100 = 0x31
    assert be.get_volume() == 50 and be.get_input() == "Mixer"
    assert be.raw("pwr1").startswith("pwr1")

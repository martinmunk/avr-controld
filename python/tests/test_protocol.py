"""Offline tests for the protocol dispatch + socket-path helper."""

from avrcontrold import protocol as P
from fakes import FakeBackend


def d(be, line, ceiling=100, **kw):
    return P.dispatch(be, line, ceiling, **kw)


def test_socket_path_matches_cpp_convention(monkeypatch, tmp_path):
    monkeypatch.setenv("AVR_SOCKET_DIR", str(tmp_path))
    p = P.avr_socket_path("nad_serial", {"device": "/dev/ttyUSB0"})
    assert p == f"{tmp_path}/nad_serial__dev_ttyUSB0.sock"
    p2 = P.avr_socket_path("denon_ip", {"host": "192.168.20.216", "port": "23"})
    assert p2 == f"{tmp_path}/denon_ip_192_168_20_216_23.sock"


def test_basic_verbs():
    be = FakeBackend(volume=40, mute=False, power=True)
    assert d(be, "PING") == "OK"
    assert d(be, "NAME") == "OK Fake"
    assert d(be, "CONNECTED") == "OK 1"
    assert d(be, "CAPS").startswith("OK abs=1 query=1 mute=1 input=1")


def test_get_set_roundtrip():
    be = FakeBackend(volume=40)
    assert d(be, "GET volume") == "OK 40"
    assert d(be, "SET volume 55") == "OK"
    assert d(be, "GET volume") == "OK 55"
    assert d(be, "GET mute") == "OK 0"
    assert d(be, "SET mute 1") == "OK"
    assert d(be, "GET mute") == "OK 1"


def test_set_volume_is_clamped():
    be = FakeBackend(volume=10)
    assert d(be, "SET volume 100", ceiling=40) == "OK"
    assert be.get_volume() == 40           # clamped by the daemon ceiling


def test_relative_volume():
    be = FakeBackend(volume=50)
    assert d(be, "VOLUP") == "OK"
    assert d(be, "VOLDOWN") == "OK"
    assert be.get_volume() == 50


def test_inputs_and_input_select():
    be = FakeBackend(inputs=["TV", "CD"], current_input="TV")
    assert d(be, "INPUTS") == "OK TV,CD"
    assert d(be, "SET input CD") == "OK"
    assert d(be, "GET input") == "OK CD"


def test_unknown_state_is_dash():
    be = FakeBackend(connected=False)
    assert d(be, "GET volume") == "OK -"


def test_meta_and_playstate_providers():
    be = FakeBackend()
    assert d(be, "META", meta_provider=lambda: "title=x|artist=y") == "OK title=x|artist=y"
    assert d(be, "PLAYSTATE", playstate_provider=lambda: "Playing") == "OK Playing"
    assert d(be, "META") == "OK -"


def test_bad_and_unknown():
    be = FakeBackend()
    assert d(be, "SET volume") == "ERR bad value"
    assert d(be, "SET volume loud") == "ERR bad value"
    assert d(be, "GET bogus") == "ERR bad GET"
    assert d(be, "FLOOP") == "ERR unknown verb"


def test_demand_verb():
    be = FakeBackend()
    seen = []
    h = lambda name, active, inp: seen.append((name, active, inp))
    assert P.dispatch(be, "DEMAND cec 1 Digital 1", 100, demand_handler=h) == "OK"
    assert seen[-1] == ("cec", True, "Digital 1")
    assert P.dispatch(be, "DEMAND airplay 0", 100, demand_handler=h) == "OK"
    assert seen[-1] == ("airplay", False, None)
    # no handler -> benign
    assert P.dispatch(be, "DEMAND cec 1", 100) == "OK -"
    assert P.dispatch(be, "DEMAND", 100, demand_handler=h) == "ERR bad DEMAND"

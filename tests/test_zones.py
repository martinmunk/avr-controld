"""Offline tests for zone mapping (AirPlay endpoint -> receiver zone).

Pure wiring around a FakeBackend: no D-Bus, no threads. Verifies routing (Main
core vs. an extra zone's set_extra), the per-zone volume ceiling, mute handling,
and that an idle endpoint can't move the receiver.
"""

from avrcontrold.zones import make_zone_target
from fakes import FakeBackend


def test_main_zone_routes_to_core_methods():
    b = FakeBackend(inputs=["TV", "AirPlay"])
    zt = make_zone_target("main", b, name="livingroom", ceiling=100,
                          source="AirPlay")
    zt.on_playing(True)
    zt.on_volume(-15.0, playing=True)   # -15 dB over [-30,0] -> 50%
    assert ("set_power", True) in b.calls
    assert ("set_input", "AirPlay") in b.calls
    assert ("set_volume", 50) in b.calls


def test_extra_zone_routes_to_set_extra():
    b = FakeBackend()
    zt = make_zone_target("zone2", b, name="diningroom", ceiling=100,
                          source="AirPlay 2")
    zt.on_playing(True)
    zt.on_volume(-15.0, playing=True)
    assert ("set_extra", "zone2_power", True) in b.calls
    assert ("set_extra", "zone2_source", "AirPlay 2") in b.calls
    assert ("set_extra", "zone2_volume", 50) in b.calls


def test_ceiling_clamps_volume_per_zone():
    b = FakeBackend()
    zt = make_zone_target("main", b, name="lr", ceiling=30)
    zt.on_volume(0.0, playing=True)     # 0 dB -> 100% requested
    assert ("set_volume", 30) in b.calls  # clamped to the zone ceiling
    assert not any(c == ("set_volume", 100) for c in b.calls)


def test_extra_zone_ceiling_clamps():
    b = FakeBackend()
    zt = make_zone_target("zone2", b, name="dr", ceiling=40)
    zt.on_volume(0.0, playing=True)
    assert ("set_extra", "zone2_volume", 40) in b.calls


def test_idle_endpoint_cannot_move_volume():
    b = FakeBackend()
    zt = make_zone_target("main", b, name="lr", ceiling=100)
    zt.on_volume(-3.0, playing=False)
    assert not any(c[0] == "set_volume" for c in b.calls)


def test_mute_and_recovery():
    b = FakeBackend()
    zt = make_zone_target("main", b, name="lr", ceiling=100)
    zt.on_volume(-144.0, playing=True)          # shairport mute sentinel
    assert ("set_mute", True) in b.calls
    b.calls.clear()
    zt.on_volume(-144.0, playing=True)          # still muted -> no repeat
    assert ("set_mute", True) not in b.calls
    zt.on_volume(-10.0, playing=True)           # back to audible -> unmute + set
    assert ("set_mute", False) in b.calls
    assert any(c[0] == "set_volume" for c in b.calls)


def test_extra_zone_mute_routes_to_set_extra():
    b = FakeBackend()
    zt = make_zone_target("zone2", b, name="dr", ceiling=100)
    zt.on_volume(-144.0, playing=True)
    assert ("set_extra", "zone2_mute", True) in b.calls


def test_stop_powers_zone_off():
    b = FakeBackend()
    zt = make_zone_target("zone2", b, name="dr", ceiling=100, source="AirPlay 2")
    zt.on_playing(False)
    assert ("set_extra", "zone2_power", False) in b.calls
    # source is only (re)selected on start, not on stop
    assert not any(c == ("set_extra", "zone2_source", "AirPlay 2") for c in b.calls)
# ---- shared import for the zone-target unit tests below --------------------
from avrcontrold.zones import ZoneTarget



# ---- input-stolen detection (drop AirPlay session on input switch) ---------
def _src_zone(cur_src_box, dropped, source="AirPlay"):
    return ZoneTarget(
        name="z", set_volume=lambda p: True, set_mute=lambda m: True,
        set_power=lambda o: True, set_source=lambda s: True, ceiling=100,
        source=source, read_source=lambda: cur_src_box[0],
    )


def _drop_counter(dropped):
    def d():
        dropped.append(1); return True
    return d


def test_input_not_dropped_when_disconnected():
    cur = ["TV"]; dropped = []
    zt = _src_zone(cur, dropped)
    zt.check_input_stolen(False, _drop_counter(dropped), now=100.0)
    assert dropped == []


def test_input_not_dropped_when_source_matches():
    cur = ["AirPlay"]; dropped = []
    zt = _src_zone(cur, dropped)
    zt.check_input_stolen(True, _drop_counter(dropped), now=100.0)
    assert dropped == []


def test_input_dropped_once_when_stolen():
    cur = ["TV"]; dropped = []
    zt = _src_zone(cur, dropped)
    zt.check_input_stolen(True, _drop_counter(dropped), now=100.0)
    assert dropped == [1]
    zt.check_input_stolen(True, _drop_counter(dropped), now=103.0)   # still TV
    assert dropped == [1]                                            # not re-dropped


def test_input_grace_after_connect():
    cur = ["TV"]; dropped = []
    zt = _src_zone(cur, dropped)
    # simulate a fresh connect: disconnected sets a grace window
    zt.check_input_stolen(False, _drop_counter(dropped), now=100.0)  # grace -> 104
    zt.check_input_stolen(True, _drop_counter(dropped), now=102.0)   # within grace
    assert dropped == []
    zt.check_input_stolen(True, _drop_counter(dropped), now=105.0)   # grace passed
    assert dropped == [1]


def test_input_recovers_when_source_returns():
    cur = ["TV"]; dropped = []
    zt = _src_zone(cur, dropped)
    zt.check_input_stolen(True, _drop_counter(dropped), now=100.0)
    assert dropped == [1]
    cur[0] = "AirPlay"                                   # user switched back
    zt.check_input_stolen(True, _drop_counter(dropped), now=103.0)
    cur[0] = "TV"                                        # and away again
    zt.check_input_stolen(True, _drop_counter(dropped), now=106.0)
    assert dropped == [1, 1]                             # dropped again after recovery


def test_zone_curve_applies_and_ceiling_still_clamps():
    b = FakeBackend()
    zt = make_zone_target("main", b, name="z", ceiling=60, airplay_curve="standard")
    zt.on_volume(-20.0, playing=True)   # standard taper: a third -> 55
    assert ("set_volume", 55) in b.calls
    zt.on_volume(-10.0, playing=True)   # standard: 85 -> clamped to 60
    assert ("set_volume", 60) in b.calls
    assert not any(c == ("set_volume", 85) for c in b.calls)

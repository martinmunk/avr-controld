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


# ---- reverse sync (receiver remote -> phone slider) ------------------------
from avrcontrold.volume import airplay_db_to_pct, airplay_pct_to_db
from avrcontrold.zones import ZoneTarget


def _rev_zone(cur_pct_box, pushed):
    """A ZoneTarget wired with stub read/push for reverse-sync tests."""
    return ZoneTarget(
        name="z", set_volume=lambda p: True, set_mute=lambda m: True,
        set_power=lambda o: True, set_source=None, ceiling=100,
        read_volume=lambda: cur_pct_box[0],
        push_phone=lambda db: (pushed.append(db) or True),
    )


def test_airplay_pct_to_db_inverts_db_to_pct():
    for db in (-30.0, -20.0, -10.0, 0.0):
        assert abs(airplay_pct_to_db(airplay_db_to_pct(db)) - db) < 0.5


def test_reverse_no_push_when_disconnected():
    cur = [50]; pushed = []
    zt = _rev_zone(cur, pushed)
    zt.sync_from_receiver(connected=False)
    assert pushed == []


def test_reverse_first_read_is_baseline():
    cur = [50]; pushed = []
    zt = _rev_zone(cur, pushed)
    zt.sync_from_receiver(True)          # baseline only
    assert pushed == []
    zt.sync_from_receiver(True)          # unchanged -> still nothing
    assert pushed == []


def test_reverse_small_change_within_deadband_ignored():
    cur = [50]; pushed = []
    zt = _rev_zone(cur, pushed)
    zt.sync_from_receiver(True)          # baseline 50
    cur[0] = 51                          # +1, within deadband
    zt.sync_from_receiver(True)
    assert pushed == []


def test_reverse_real_change_pushes_mapped_db():
    cur = [50]; pushed = []
    zt = _rev_zone(cur, pushed)
    zt.sync_from_receiver(True)          # baseline 50
    cur[0] = 100                         # remote turned it up
    zt.sync_from_receiver(True)
    assert len(pushed) == 1
    assert abs(pushed[0] - airplay_pct_to_db(100)) < 0.01   # 0 dB at 100%


def test_reverse_suppresses_echo_of_our_own_set():
    cur = [30]; pushed = []
    zt = _rev_zone(cur, pushed)
    zt.sync_from_receiver(True)          # baseline 30
    # phone -> receiver: on_volume drives the receiver and records the level
    zt.on_volume(-15.0, playing=True)    # airplay -15 over [-30,0] -> 50%
    cur[0] = 50                          # receiver now reads what we set
    zt.sync_from_receiver(True)
    assert pushed == []                  # our own change is not echoed back

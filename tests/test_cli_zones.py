"""Config parsing for multi-zone AirPlay watchers (parse_zones)."""

from avrcontrold.cli import parse_zones


def test_no_zones_key_returns_empty():
    assert parse_zones({}) == []
    assert parse_zones({"shairport": "1"}) == []


def test_two_zones_with_defaults_and_overrides():
    cfg = {
        "zones": "livingroom, diningroom",
        "zone.livingroom.bus": "unix:path=/run/pi-audio/livingroom/bus",
        "zone.livingroom.target": "Main",           # case-normalised
        "zone.livingroom.input": "AirPlay",
        "zone.diningroom.bus": "unix:path=/run/pi-audio/diningroom/bus",
        "zone.diningroom.target": "zone2",
        "zone.diningroom.input": "AirPlay 2",
        "zone.diningroom.max_volume": "60",
    }
    zones = parse_zones(cfg)
    assert [z["name"] for z in zones] == ["livingroom", "diningroom"]
    lr, dr = zones
    assert lr["target"] == "main"
    assert lr["bus"] == "unix:path=/run/pi-audio/livingroom/bus"
    assert lr["max_volume"] == ""          # inherits global ceiling
    assert dr["target"] == "zone2"
    assert dr["max_volume"] == "60"


def test_missing_bus_defaults_to_system():
    cfg = {"zones": "solo"}
    (z,) = parse_zones(cfg)
    assert z["bus"] == "SYSTEM"
    assert z["target"] == "main"

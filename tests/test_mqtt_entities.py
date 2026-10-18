"""Offline tests for the HA MQTT-discovery entity model (no broker)."""

import json

from avrcontrold.backends.base import Capabilities
from avrcontrold.mqtt import entities as E


DEV = E.Device(node_id="nad_serial__dev_ttyUSB0", name="Living Room NAD",
               manufacturer="NAD", model="T757")

FULL = Capabilities(absolute=True, query=True, mute=True, input_select=True,
                    power=True, min_volume=0, max_volume=100)


def _by_key(ents):
    return {e.key: e for e in ents}


def test_full_capabilities_produce_expected_entities():
    ents = _by_key(E.build_entities(DEV, FULL, inputs=["TV", "CD"]))
    assert set(ents) == {"power", "volume", "mute", "input"}
    assert ents["power"].component == "switch"
    assert ents["volume"].component == "number"
    assert ents["mute"].component == "switch"
    assert ents["input"].component == "select"


def test_capabilities_gate_entities():
    caps = Capabilities(absolute=False, mute=True, input_select=False, power=False)
    ents = _by_key(E.build_entities(DEV, caps))
    assert set(ents) == {"mute"}          # only mute is capable


def test_number_uses_capability_range():
    ents = _by_key(E.build_entities(DEV, Capabilities(absolute=True,
                                                      min_volume=0, max_volume=80)))
    cfg = ents["volume"].config
    assert cfg["min"] == 0 and cfg["max"] == 80 and cfg["step"] == 1


def test_select_lists_inputs():
    ents = _by_key(E.build_entities(DEV, FULL, inputs=["TV", "CD", "Stream"]))
    assert ents["input"].config["options"] == ["TV", "CD", "Stream"]


def test_topics_and_unique_ids():
    ents = _by_key(E.build_entities(DEV, FULL, inputs=["TV"]))
    vol = ents["volume"]
    assert vol.state_topic == "avrcontrold/nad_serial__dev_ttyUSB0/volume/state"
    assert vol.command_topic == "avrcontrold/nad_serial__dev_ttyUSB0/volume/set"
    assert vol.config["unique_id"] == "avrcontrold_nad_serial__dev_ttyUSB0_volume"
    assert vol.config["availability_topic"] == \
        "avrcontrold/nad_serial__dev_ttyUSB0/availability"


def test_discovery_topic_format():
    ents = _by_key(E.build_entities(DEV, FULL, inputs=["TV"]))
    t = E.discovery_topic("homeassistant", ents["power"], DEV.node_id)
    assert t == "homeassistant/switch/nad_serial__dev_ttyUSB0/power/config"


def test_discovery_payload_is_valid_json_with_device():
    ents = _by_key(E.build_entities(DEV, FULL, inputs=["TV"]))
    payload = json.loads(E.discovery_payload(ents["power"]))
    assert payload["device"]["identifiers"] == ["avrcontrold_nad_serial__dev_ttyUSB0"]
    assert payload["device"]["manufacturer"] == "NAD"
    assert payload["command_topic"].endswith("/power/set")


def test_encode_state():
    assert E.encode_state("power", True) == "ON"
    assert E.encode_state("mute", False) == "OFF"
    assert E.encode_state("volume", 42) == "42"
    assert E.encode_state("input", "TV") == "TV"
    assert E.encode_state("volume", None) is None       # unknown -> no publish


def test_decode_command():
    assert E.decode_command("power", "ON") is True
    assert E.decode_command("power", "off") is False
    assert E.decode_command("mute", "1") is True
    assert E.decode_command("volume", "37") == 37
    assert E.decode_command("volume", "37.6") == 38
    assert E.decode_command("input", " TV ") == "TV"


def test_decode_command_rejects_bad_input():
    import pytest
    with pytest.raises(ValueError):
        E.decode_command("power", "maybe")
    with pytest.raises(ValueError):
        E.decode_command("bogus", "x")


def test_sanitize():
    assert E.sanitize("/dev/ttyUSB0") == "dev_ttyUSB0"
    assert E.sanitize("192.168.20.216:23") == "192_168_20_216_23"

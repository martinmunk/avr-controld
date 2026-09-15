"""Offline tests for the MQTT bridge using a fake client + fake backend."""

import json

from avrcontrold.mqtt import entities as E
from avrcontrold.mqtt.bridge import MqttBridge
from fakes import FakeBackend


DEV = E.Device(node_id="nad_serial__dev_ttyUSB0", name="Living Room NAD",
               manufacturer="NAD", model="T757")


class FakeClient:
    def __init__(self):
        self.published: list[tuple[str, str, bool]] = []
        self.subscriptions: list[str] = []

    def publish(self, topic, payload, retain=False):
        self.published.append((topic, payload, retain))

    def subscribe(self, topic):
        self.subscriptions.append(topic)

    # helpers
    def last(self, topic):
        for t, p, _ in reversed(self.published):
            if t == topic:
                return p
        return None


def make(ceiling=100, **backend_kw):
    be = FakeBackend(**backend_kw)
    cl = FakeClient()
    br = MqttBridge(DEV, be, cl, ceiling=ceiling)
    return be, cl, br


def test_announce_publishes_discovery_and_subscribes():
    be, cl, br = make(volume=40, current_input="TV")
    br.announce()
    # discovery configs are retained
    disc = [t for t, _, r in cl.published if t.endswith("/config") and r]
    assert any("/switch/nad_serial__dev_ttyUSB0/power/config" in t for t in disc)
    assert any("/number/nad_serial__dev_ttyUSB0/volume/config" in t for t in disc)
    # commands subscribed
    assert "avrcontrold/nad_serial__dev_ttyUSB0/volume/set" in cl.subscriptions
    # availability online
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/availability") == "online"
    # initial state published
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/volume/state") == "40"
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/power/state") == "ON"


def test_command_sets_backend_and_reflects_state():
    be, cl, br = make(volume=40)
    br.announce()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/volume/set", "55")
    assert be.get_volume() == 55
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/volume/state") == "55"


def test_volume_command_is_clamped_to_ceiling():
    be, cl, br = make(ceiling=40, volume=30)
    br.announce()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/volume/set", "100")
    assert be.get_volume() == 40          # clamped, not 100
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/volume/state") == "40"


def test_mute_and_power_commands():
    be, cl, br = make(mute=False, power=True)
    br.announce()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/mute/set", "ON")
    assert be.get_mute() is True
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/power/set", "OFF")
    assert be.get_power() is False


def test_input_command():
    be, cl, br = make(inputs=["TV", "CD"], current_input="TV")
    br.announce()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/input/set", "CD")
    assert be.get_input() == "CD"
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/input/state") == "CD"


def test_bad_command_payload_is_ignored():
    be, cl, br = make(volume=40)
    br.announce()
    before = be.get_volume()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/volume/set", "loud")
    assert be.get_volume() == before      # unchanged


def test_unmapped_topic_ignored():
    be, cl, br = make()
    br.announce()
    n = len(be.calls)
    br.handle_message("some/other/topic", "x")
    assert len(be.calls) == n


def test_publish_state_deduplicates():
    be, cl, br = make(volume=40)
    br.announce()
    n = len(cl.published)
    br.publish_state()                    # nothing changed
    assert len(cl.published) == n
    be.set_volume(41)
    br.publish_state()                    # one change -> one publish
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/volume/state") == "41"


def test_go_offline():
    be, cl, br = make()
    br.announce()
    br.set_availability(False)
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/availability") == "offline"


def test_discovery_payload_valid_json():
    be, cl, br = make()
    br.announce()
    for t, p, _ in cl.published:
        if t.endswith("/config"):
            json.loads(p)                 # must be valid JSON


def test_extra_entities_published_and_states():
    be, cl, br = make()
    br.announce()
    disc = [t for t, _, r in cl.published if t.endswith("/config")]
    assert any("/number/nad_serial__dev_ttyUSB0/bass/config" in t for t in disc)
    assert any("/sensor/nad_serial__dev_ttyUSB0/model/config" in t for t in disc)
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/bass/state") == "0"
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/model/state") == "FakeAmp"


def test_extra_command_sets_backend():
    be, cl, br = make()
    br.announce()
    br.handle_message("avrcontrold/nad_serial__dev_ttyUSB0/bass/set", "5")
    assert be.get_extra("bass") == 5
    assert cl.last("avrcontrold/nad_serial__dev_ttyUSB0/bass/state") == "5"


def test_extra_sensor_is_read_only():
    be, cl, br = make()
    br.announce()
    assert not any(t.endswith("/model/set") for t in cl.subscriptions)
    assert any(t.endswith("/bass/set") for t in cl.subscriptions)   # number is writable

from avrcontrold.cli import mqtt_enabled


def test_mqtt_disabled_without_host():
    assert mqtt_enabled({}) is False
    assert mqtt_enabled({"mqtt_host": ""}) is False
    assert mqtt_enabled({"mqtt_host": "   "}) is False


def test_mqtt_enabled_with_host():
    assert mqtt_enabled({"mqtt_host": "mqtt.service"}) is True
    assert mqtt_enabled({"mqtt_host": " 10.0.0.2 "}) is True

"""paho-mqtt transport implementing the MqttClient protocol.

Handles the network thread, an availability last-will (so HA marks the device
unavailable if we die), and re-announcing on every (re)connect. Backend/bridge
access is serialised by the caller (cli), since paho callbacks run on the
network thread.
"""

from __future__ import annotations

import logging
from typing import Callable

import paho.mqtt.client as mqtt

from .entities import availability_topic

log = logging.getLogger(__name__)


class PahoTransport:
    def __init__(self, host: str, port: int, node_id: str, *,
                 username: str | None = None, password: str | None = None,
                 client_id: str | None = None, keepalive: int = 30):
        self.host = host
        self.port = port
        self.keepalive = keepalive
        self._node_id = node_id
        #: set by the caller; invoked (topic, payload) on each message
        self.on_message: Callable[[str, str], None] | None = None
        #: set by the caller; invoked with no args on each successful connect
        self.on_connect: Callable[[], None] | None = None

        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                        client_id=client_id or f"avrcontrold-{node_id}")
        if username:
            c.username_pw_set(username, password or "")
        c.will_set(availability_topic(node_id), "offline", retain=True)
        c.on_connect = self._handle_connect
        c.on_message = self._handle_message
        c.on_disconnect = self._handle_disconnect
        self._c = c

    # ---- paho callbacks -------------------------------------------------
    def _handle_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code.is_failure:
            log.warning("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected to %s:%s", self.host, self.port)
        if self.on_connect:
            self.on_connect()

    def _handle_disconnect(self, client, userdata, flags, reason_code, properties=None):
        log.warning("MQTT disconnected: %s (auto-reconnect)", reason_code)

    def _handle_message(self, client, userdata, msg):
        try:
            payload = msg.payload.decode("utf-8", "replace")
        except Exception:
            return
        if self.on_message:
            self.on_message(msg.topic, payload)

    # ---- MqttClient protocol -------------------------------------------
    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        self._c.publish(topic, payload, retain=retain)

    def subscribe(self, topic: str) -> None:
        self._c.subscribe(topic)

    # ---- lifecycle ------------------------------------------------------
    def start(self) -> None:
        self._c.reconnect_delay_set(min_delay=1, max_delay=30)
        self._c.connect(self.host, self.port, self.keepalive)
        self._c.loop_start()

    def stop(self) -> None:
        try:
            self._c.publish(availability_topic(self._node_id), "offline", retain=True)
            self._c.disconnect()
        finally:
            self._c.loop_stop()

"""Wire the entity model to an MQTT client and a backend.

Transport-agnostic: the bridge calls an injected client's ``publish`` /
``subscribe`` and is fed incoming messages via :meth:`handle_message`, so it is
fully testable with a fake client and a fake backend. A thin paho adapter
(added separately) supplies the real transport.

The volume command path clamps through :func:`avrcontrold.volume.clamp_absolute`
with the configured ceiling -- the same last-line-of-defence the socket protocol
uses, so a bad MQTT number can never blast the receiver.
"""

from __future__ import annotations

import logging
from typing import Protocol

from ..backends.base import Backend
from ..volume import clamp_absolute
from . import entities as E

log = logging.getLogger(__name__)


class MqttClient(Protocol):
    def publish(self, topic: str, payload: str, retain: bool = False) -> None: ...
    def subscribe(self, topic: str) -> None: ...


# key -> how to read it from the backend
def _read(backend: Backend, key: str):
    return {
        "power": backend.get_power,
        "volume": backend.get_volume,
        "mute": backend.get_mute,
        "input": backend.get_input,
    }[key]()


class MqttBridge:
    def __init__(self, device: E.Device, backend: Backend, client: MqttClient,
                 *, ceiling: int = 100, discovery_prefix: str = "homeassistant",
                 on_demand=None, lock=None):
        self.device = device
        self.backend = backend
        self.client = client
        self.ceiling = ceiling
        self.prefix = discovery_prefix
        # Optional serial lock: when set, publish_state takes it per backend read
        # and releases between reads, so a long state sweep over a slow serial
        # port doesn't block interactive commands (CEC/phone volume) waiting on
        # the same lock. When None, reads are unguarded (caller-serialised).
        from contextlib import nullcontext
        self._lock = lock if lock is not None else nullcontext()
        # Generic power-policy hook: HA can publish avrcontrold/<node>/demand/<name>
        # = ON/OFF (e.g. "tv" when the TV/Shield is active) to keep the receiver
        # powered. Backend-agnostic; the daemon feeds these to the PowerPolicy.
        self.on_demand = on_demand
        self._extra_specs = {s.key: s for s in backend.extras()}
        self.entities = (E.build_entities(device, backend.capabilities(),
                                          backend.get_inputs())
                         + E.build_extra_entities(device, backend.extras()))
        self._by_cmd = {e.command_topic: e for e in self.entities if e.command_topic}
        self._demand_prefix = E.base_topic(device.node_id) + "/demand/"
        self._last: dict[str, str] = {}

    # ---- lifecycle ------------------------------------------------------
    def announce(self) -> None:
        """Publish discovery configs (retained), subscribe commands, go online."""
        for e in self.entities:
            self.client.publish(E.discovery_topic(self.prefix, e, self.device.node_id),
                                E.discovery_payload(e), retain=True)
            if e.command_topic:
                self.client.subscribe(e.command_topic)
        if self.on_demand:
            self.client.subscribe(self._demand_prefix + "+")
        self.set_availability(True)
        self.publish_state(force=True)

    def set_availability(self, online: bool) -> None:
        self.client.publish(E.availability_topic(self.device.node_id),
                            "online" if online else "offline", retain=True)

    # ---- state out ------------------------------------------------------
    def publish_state(self, force: bool = False) -> None:
        for e in self.entities:
            # Take the serial lock only around the read, then release it before
            # the next entity, so interactive socket commands can interleave.
            with self._lock:
                if e.key in self._extra_specs:
                    payload = E.encode_extra(e.component, self.backend.get_extra(e.key))
                else:
                    payload = E.encode_state(e.key, _read(self.backend, e.key))
            if payload is None:
                continue
            if not force and self._last.get(e.key) == payload:
                continue
            self._last[e.key] = payload
            self.client.publish(e.state_topic, payload, retain=True)

    # ---- commands in ----------------------------------------------------
    def handle_message(self, topic: str, payload: str) -> None:
        if self.on_demand and topic.startswith(self._demand_prefix):
            name = topic[len(self._demand_prefix):]
            active = payload.strip().upper() in ("ON", "1", "TRUE")
            self.on_demand(name, active)
            return
        ent = self._by_cmd.get(topic)
        if ent is None:
            log.debug("ignoring message on unmapped topic %s", topic)
            return
        if ent.key in self._extra_specs:
            try:
                value = E.decode_extra(ent.component, payload)
            except ValueError as exc:
                log.warning("bad extra command on %s: %s", topic, exc)
                return
            self.backend.set_extra(ent.key, value)
        else:
            try:
                value = E.decode_command(ent.key, payload)
            except ValueError as exc:
                log.warning("bad command on %s: %s", topic, exc)
                return
            self._apply(ent.key, value)
        # Reflect the resulting state promptly.
        self.publish_state()

    def _apply(self, key: str, value) -> None:
        be = self.backend
        if key == "power":
            be.set_power(bool(value))
        elif key == "mute":
            be.set_mute(bool(value))
        elif key == "input":
            be.set_input(str(value))
        elif key == "volume":
            safe = clamp_absolute(int(value), self.ceiling)
            if safe != value:
                log.warning("volume %s clamped to ceiling -> %s", value, safe)
            be.set_volume(safe)

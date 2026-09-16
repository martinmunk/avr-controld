"""Home Assistant MQTT-discovery entity model (pure, dependency-free).

HA has no MQTT ``media_player`` platform, so a receiver is exposed as a small
set of primitive entities grouped under one HA *device*:

    power   -> switch
    volume  -> number   (0-100)
    mute    -> switch
    input   -> select
    (later) now-playing -> sensor, playing -> binary_sensor

Everything here is pure data construction: given a device identity, the backend
capabilities and its input list, it produces the discovery configs, the topic
names, and the state/command payload (de)serialisation. No MQTT client, no I/O,
so it is fully unit-testable in a pipeline. The bridge wires it to a real client.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

from ..backends.base import Capabilities

PAYLOAD_ON = "ON"
PAYLOAD_OFF = "OFF"


def sanitize(s: str) -> str:
    """Node-id safe: keep alnum, collapse everything else to '_'."""
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")


@dataclass(frozen=True)
class Device:
    """Identity of one receiver as an HA device."""
    node_id: str                 # unique, topic-safe (e.g. nad_serial__dev_ttyUSB0)
    name: str                    # friendly, e.g. "Living Room NAD"
    manufacturer: str = ""
    model: str = ""

    def ha_device(self) -> dict:
        d = {"identifiers": [f"avrcontrold_{self.node_id}"], "name": self.name}
        if self.manufacturer:
            d["manufacturer"] = self.manufacturer
        if self.model:
            d["model"] = self.model
        return d


@dataclass(frozen=True)
class Entity:
    """One HA entity: its component, keys, topics and discovery config."""
    component: str               # switch | number | select | sensor | binary_sensor
    key: str                     # power | volume | mute | input | ...
    config: dict = field(default_factory=dict)
    state_topic: str = ""
    command_topic: str | None = None   # None => read-only


def base_topic(node_id: str) -> str:
    return f"avrcontrold/{node_id}"


def availability_topic(node_id: str) -> str:
    return f"{base_topic(node_id)}/availability"


def discovery_topic(prefix: str, entity: Entity, node_id: str) -> str:
    """`<prefix>/<component>/<node_id>/<key>/config`."""
    return f"{prefix}/{entity.component}/{node_id}/{entity.key}/config"


def _common(device: Device, key: str, component: str) -> tuple[dict, str, str]:
    node = device.node_id
    state = f"{base_topic(node)}/{key}/state"
    cmd = f"{base_topic(node)}/{key}/set"
    cfg = {
        "name": key.replace("_", " ").title(),
        "unique_id": f"avrcontrold_{node}_{key}",
        "object_id": f"{node}_{key}",
        "state_topic": state,
        "availability_topic": availability_topic(node),
        "payload_available": "online",
        "payload_not_available": "offline",
        "device": device.ha_device(),
    }
    return cfg, state, cmd


def build_entities(device: Device, caps: Capabilities,
                   inputs: list[str] | None = None) -> list[Entity]:
    """The set of HA entities a backend exposes, per its capabilities."""
    ents: list[Entity] = []

    if caps.power:
        cfg, state, cmd = _common(device, "power", "switch")
        cfg.update({"command_topic": cmd, "payload_on": PAYLOAD_ON,
                    "payload_off": PAYLOAD_OFF, "icon": "mdi:power"})
        ents.append(Entity("switch", "power", cfg, state, cmd))

    if caps.absolute:
        cfg, state, cmd = _common(device, "volume", "number")
        cfg.update({"command_topic": cmd, "min": caps.min_volume,
                    "max": caps.max_volume, "step": 1, "mode": "slider",
                    "icon": "mdi:volume-high"})
        ents.append(Entity("number", "volume", cfg, state, cmd))

    if caps.mute:
        cfg, state, cmd = _common(device, "mute", "switch")
        cfg.update({"command_topic": cmd, "payload_on": PAYLOAD_ON,
                    "payload_off": PAYLOAD_OFF, "icon": "mdi:volume-off"})
        ents.append(Entity("switch", "mute", cfg, state, cmd))

    if caps.input_select:
        cfg, state, cmd = _common(device, "input", "select")
        cfg.update({"command_topic": cmd, "options": list(inputs or []),
                    "icon": "mdi:import"})
        ents.append(Entity("select", "input", cfg, state, cmd))

    return ents


def build_extra_entities(device: Device, specs) -> list[Entity]:
    """HA entities for a backend's extra properties (ExtraSpec list)."""
    ents: list[Entity] = []
    for s in specs:
        cfg, state, cmd = _common(device, s.key, s.component)
        cfg["name"] = s.name
        if s.icon:
            cfg["icon"] = s.icon
        if s.component == "number":
            if s.min is not None:
                cfg["min"] = s.min
            if s.max is not None:
                cfg["max"] = s.max
            cfg["step"] = s.step or 1
            cfg["mode"] = s.mode
            if s.unit:
                cfg["unit_of_measurement"] = s.unit
        elif s.component == "select":
            cfg["options"] = list(s.options or [])
        elif s.component == "switch":
            cfg["payload_on"] = PAYLOAD_ON
            cfg["payload_off"] = PAYLOAD_OFF
        writable = s.writable and s.component != "sensor"
        if writable:
            cfg["command_topic"] = cmd
        ents.append(Entity(s.component, s.key, cfg, state, cmd if writable else None))
    return ents


def encode_extra(component: str, value) -> str | None:
    if value is None:
        return None
    if component == "switch":
        return PAYLOAD_ON if value else PAYLOAD_OFF
    return str(value)


def decode_extra(component: str, payload: str):
    p = payload.strip()
    if component == "switch":
        return p.upper() in (PAYLOAD_ON, "1", "TRUE")
    if component == "number":
        f = float(p)
        return int(f) if f.is_integer() else f
    return p


def discovery_payload(entity: Entity) -> str:
    return json.dumps(entity.config, separators=(",", ":"), sort_keys=True)


# ---- state <-> payload -----------------------------------------------------
def encode_state(key: str, value) -> str | None:
    """Backend state value -> MQTT state payload. None if unknown (no publish)."""
    if value is None:
        return None
    if key in ("power", "mute"):
        return PAYLOAD_ON if value else PAYLOAD_OFF
    if key == "volume":
        return str(int(value))
    if key == "input":
        return str(value)
    return str(value)


def decode_command(key: str, payload: str):
    """MQTT command payload -> a typed value for the backend. Raises on bad input."""
    p = payload.strip()
    if key in ("power", "mute"):
        u = p.upper()
        if u in (PAYLOAD_ON, "1", "TRUE"):
            return True
        if u in (PAYLOAD_OFF, "0", "FALSE"):
            return False
        raise ValueError(f"bad bool payload: {payload!r}")
    if key == "volume":
        return int(round(float(p)))
    if key == "input":
        return p
    raise ValueError(f"unknown command key: {key}")

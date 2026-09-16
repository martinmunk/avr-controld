"""avr-controld entry point: own a receiver and bridge it to Home Assistant.

Config comes from a key=value file (--config) overridden by -o key=value flags.
Secrets (MQTT password) should come from the file or the environment, never the
command line in production.

    avr-controld -o backend=nad -o device=/dev/ttyUSB0 \
                 -o mqtt_host=mqtt.service -o mqtt_user=... -o mqtt_pass=... \
                 -o name="Living Room NAD" -o max_volume=80

This first cut runs the MQTT bridge + a poll loop. The Unix-socket protocol
server (for the ALSA plugins and avr-cec-relay) is added on top of the same
backend next.
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import threading
import time

from .backends.base import Backend
from .backends.demo import DemoBackend
from .mqtt.bridge import MqttBridge
from .mqtt.entities import Device, sanitize
from .mqtt.paho_client import PahoTransport
from .power_policy import PowerPolicy
from .protocol import avr_socket_path
from .shairport import ShairportWatcher
from .socket_server import SocketServer
from .volume import airplay_db_to_pct, clamp_absolute, clamp_ceiling, is_airplay_mute
from .zones import make_zone_target

# Map a config backend name to the canonical socket "type" the C ALSA plugins
# and avr-cec-relay use, so the derived socket path matches theirs.
_SOCKET_TYPE = {
    "nad": "nad_serial", "nad_serial": "nad_serial",
    "denon": "denon_ip", "denon_ip": "denon_ip",
    "opalum": "opalum_serial", "opalum_serial": "opalum_serial",
    "demo": "demo",
}

log = logging.getLogger("avrcontrold")


def load_config(argv: list[str]) -> dict:
    ap = argparse.ArgumentParser(prog="avr-controld")
    ap.add_argument("--config", help="key=value config file")
    ap.add_argument("-o", action="append", default=[], metavar="key=value",
                    help="config override (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    cfg: dict[str, str] = {}
    path = args.config or os.environ.get("AVR_CONTROLD_CONFIG", "/etc/avr-controld.conf")
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
    for kv in args.o:
        if "=" in kv:
            k, v = kv.split("=", 1)
            cfg[k.strip()] = v.strip()
    cfg["_verbose"] = "1" if args.verbose else cfg.get("_verbose", "0")
    return cfg


def build_backend(cfg: dict) -> Backend:
    kind = cfg.get("backend", "nad").lower()
    if kind == "demo":
        return DemoBackend()
    if kind in ("nad", "nad_serial"):
        from .backends.nad import NADBackend
        sources = [s for s in cfg.get("sources", "").split(",") if s.strip()]
        return NADBackend(
            cfg.get("device", "/dev/ttyUSB0"),
            min_db=float(cfg.get("min_db", "-80")),
            max_db=float(cfg.get("max_db", "0")),
            sources=[s.strip() for s in sources],
        )
    raise SystemExit(f"unknown backend: {kind}")


def default_node_id(cfg: dict) -> str:
    ident = cfg.get("device") or f"{cfg.get('mqtt_host','')}"
    return sanitize(f"{cfg.get('backend','nad')}_{ident}")


def parse_zones(cfg: dict) -> list[dict]:
    """Parse multi-zone AirPlay watchers from config, for the Docker per-container
    D-Bus setup. Returns [] when no ``zones`` key is set (use the single-watcher
    path instead). Config shape::

        zones = livingroom,diningroom
        zone.livingroom.bus        = unix:path=/run/pi-audio/livingroom/bus
        zone.livingroom.target     = main
        zone.livingroom.input      = AirPlay
        zone.diningroom.bus        = unix:path=/run/pi-audio/diningroom/bus
        zone.diningroom.target     = zone2
        zone.diningroom.input      = AirPlay 2
        zone.diningroom.max_volume = 60      # optional per-zone ceiling
    """
    names = [z.strip() for z in cfg.get("zones", "").split(",") if z.strip()]
    zones: list[dict] = []
    for name in names:
        p = f"zone.{name}."
        zones.append({
            "name": name,
            "bus": cfg.get(p + "bus", "SYSTEM"),
            "target": cfg.get(p + "target", "main").lower(),
            "input": cfg.get(p + "input", ""),
            "max_volume": cfg.get(p + "max_volume", ""),  # "" => inherit global
            # volume sync (phone slider -> receiver zone). "off" leaves the zone's
            # volume alone (e.g. shairport does software volume on the endpoint),
            # driving only power/source. Default on.
            "volume": cfg.get(p + "volume", "on"),
        })
    return zones


def main(argv: list[str] | None = None) -> int:
    cfg = load_config(list(argv) if argv is not None else None)
    logging.basicConfig(
        level=logging.DEBUG if cfg.get("_verbose") == "1" else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    backend = build_backend(cfg)
    backend.connect()

    node_id = cfg.get("node_id") or default_node_id(cfg)
    device = Device(
        node_id=node_id,
        name=cfg.get("name", f"AVR {node_id}"),
        manufacturer=cfg.get("manufacturer", ""),
        model=cfg.get("model", ""),
    )
    ceiling = int(cfg.get("max_volume", "100"))
    poll_interval = float(cfg.get("poll_interval", "5"))

    transport = PahoTransport(
        host=cfg.get("mqtt_host", "localhost"),
        port=int(cfg.get("mqtt_port", "1883")),
        node_id=node_id,
        username=cfg.get("mqtt_user") or None,
        password=cfg.get("mqtt_pass") or os.environ.get("MQTT_PASS"),
    )
    # Reentrant: the socket dispatch holds the lock and calls the shairport
    # snapshot provider, which also takes it; the power callbacks nest too.
    lock = threading.RLock()

    truthy = lambda v: str(v).lower() in ("1", "on", "yes", "true")  # noqa: E731

    # ---- optional power policy (generic; not receiver-specific) ----------
    # A "demand" is anything that needs the receiver on: shairport playing, or
    # the CEC relay seeing the TV/setup active. Each demand may carry the input
    # to select when it powers the receiver on (e.g. the NAD optical the TV or
    # the Pi feeds). No Home Assistant involved -- the CEC relay feeds its demand
    # over the socket (DEMAND verb), shairport feeds its own.
    policy = None
    airplay_input = cfg.get("airplay_input", "") or cfg.get("input", "")
    if truthy(cfg.get("power_policy", "0")):
        policy = PowerPolicy(off_grace_s=float(cfg.get("off_grace", "120")))

    def feed_demand(name, active, inp=None):
        if policy is None:
            return
        with lock:
            act = policy.set_demand(name, active, time.monotonic())
            chosen = inp or (airplay_input if name == "airplay" else "")
            if act is True:
                backend.set_power(True)
                if chosen:
                    backend.set_input(chosen)
                log.info("power: ON (demand %s)%s", name,
                         f" + input {chosen}" if chosen else "")
            elif act is False:
                backend.set_power(False)
                log.info("power: OFF (no demand, grace elapsed)")
            elif active and chosen and backend.get_power():
                # Already on; a (re)activated source wants its own input.
                backend.set_input(chosen)
                log.info("input -> %s (demand %s active)", chosen, name)

    # ---- optional AirPlay volume sync (phone slider -> receiver) ---------
    airplay_min_db = float(cfg.get("airplay_min_db", "-30"))
    _airplay_muted = [False]

    def on_airplay_volume(db):
        if watcher is None:
            return
        with lock:
            if not watcher.state.playing():
                return                       # only while AirPlay is the source
            if is_airplay_mute(db):
                if not _airplay_muted[0]:
                    backend.set_mute(True)
                    _airplay_muted[0] = True
                return
            if _airplay_muted[0]:
                backend.set_mute(False)
                _airplay_muted[0] = False
            pct = clamp_absolute(airplay_db_to_pct(db, min_db=airplay_min_db), ceiling)
            backend.set_volume(pct)

    bridge = MqttBridge(device, backend, transport,
                        ceiling=ceiling,
                        discovery_prefix=cfg.get("discovery_prefix", "homeassistant"),
                        on_demand=(feed_demand if policy is not None else None))

    transport.on_connect = lambda: _locked(lock, bridge.announce)
    transport.on_message = lambda t, p: _locked(lock, lambda: bridge.handle_message(t, p))

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    # ---- shairport-sync watcher(s) (play-state -> power, slider -> volume) --
    # Two shapes:
    #  * multi-zone (Docker): ``zones=...`` -> one watcher per zone, each on its
    #    own bus, routed to a receiver zone (Main core, or Zone 2 extras).
    #  * single (legacy StueTærten): ``shairport=1`` -> one Main watcher on the
    #    system bus, driving the power policy + Main volume as before.
    # ``watcher`` is the primary (first Main, else first) used for the socket
    # meta/playstate providers; ``watchers`` is stopped as a set on shutdown.
    watchers: list[ShairportWatcher] = []
    watcher = None
    zones = parse_zones(cfg)
    if zones:
        for z in zones:
            zceil = clamp_ceiling(int(z["max_volume"])) if z["max_volume"] else ceiling
            zt = make_zone_target(z["target"], backend, name=z["name"],
                                  ceiling=zceil, source=z["input"],
                                  airplay_min_db=airplay_min_db)
            w = ShairportWatcher(bus=z["bus"], label=z["name"], lock=lock)
            # Main zone routes power through the shared policy (so the CEC/TV
            # demand and AirPlay demand cooperate); extra zones own their power.
            if z["target"] == "main" and policy is not None:
                w.on_playing = (lambda p, zn=z["name"], inp=z["input"]:
                                feed_demand(f"airplay:{zn}", p, inp))
            else:
                w.on_playing = zt.on_playing
            # Volume sync is on by default (phone slider -> receiver zone volume,
            # clamped to the zone ceiling). volume=off leaves the zone's volume
            # untouched (endpoint does its own software volume).
            if truthy(z.get("volume", "on")):
                w.on_volume = (lambda db, zt=zt, w=w: zt.on_volume(db, w.state.playing()))
            watchers.append(w)
            if watcher is None or z["target"] == "main":
                watcher = w
        log.info("zones: %s", ", ".join(f"{z['name']}->{z['target']}" for z in zones))
    elif truthy(cfg.get("shairport", "0")):
        watcher = ShairportWatcher(
            on_playing=lambda p: feed_demand("airplay", p),
            on_volume=on_airplay_volume,
            lock=lock,
        )
        watchers.append(watcher)
    for w in watchers:
        w.start()

    # ---- Unix-socket server (plugins + avr-cec-relay bind here) ----------
    kind = cfg.get("backend", "nad").lower()
    stype = cfg.get("socket_type") or _SOCKET_TYPE.get(kind, kind)
    socket_path = cfg.get("avr_socket") or avr_socket_path(stype, cfg)
    srv = SocketServer(socket_path, backend, lock, ceiling, on_shutdown=stop.set,
                       meta_provider=(watcher.snapshot if watcher else None),
                       playstate_provider=(watcher.playstate if watcher else None),
                       demand_handler=(feed_demand if policy is not None else None))
    srv_ok = srv.start_listening()
    if srv_ok:
        srv.start()
    else:
        log.warning("socket server disabled (bind failed on %s)", socket_path)

    log.info("starting: backend=%s node_id=%s mqtt=%s:%s ceiling=%s socket=%s "
             "power_policy=%s shairport=%s", backend.name, node_id, transport.host,
             transport.port, ceiling, socket_path if srv_ok else "(none)",
             policy is not None, watcher is not None)
    transport.start()
    try:
        while not stop.wait(poll_interval):
            with lock:
                if not backend.is_connected():
                    backend.connect()
                ev = backend.poll_events()
                if not ev.empty():
                    log.debug("push event: %s", ev)
                if policy is not None and policy.tick(time.monotonic()) is False:
                    backend.set_power(False)
                    log.info("power: OFF (no demand, grace elapsed)")
                bridge.publish_state()
    finally:
        for w in watchers:
            w.stop()
        if srv_ok:
            srv.stop()
        with lock:
            bridge.set_availability(False)
        transport.stop()
        backend.close()
    return 0


def _locked(lock, fn):
    with lock:
        fn()


if __name__ == "__main__":
    raise SystemExit(main())

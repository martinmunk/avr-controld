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
from .power_policy import PowerPolicy
from .protocol import avr_socket_path
from .shairport import ShairportWatcher
from .socket_server import SocketServer
from .volume import (AIRPLAY_CURVES, airplay_db_to_pct, clamp_absolute, clamp_ceiling,
                     is_airplay_mute)
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
    if kind in ("opalum", "opalum_serial"):
        from .backends.opalum import OpalumBackend
        return OpalumBackend(cfg.get("device", "/dev/ttyUSB0"))
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


def own_receiver_power(backend: Backend, cfg: dict) -> bool | None:
    """When the daemon's power policy owns the receiver's power, the receiver's
    own idle/auto-standby timer must be off: it powers the unit down behind the
    policy's back (the NAD T757 did exactly that under a live TV, ~30-45 min
    after the source went silent) and the policy -- which acts on demand edges
    only -- never notices. Backend-agnostic: applies to any backend exposing an
    `auto_standby` extra. `auto_standby = keep` in the config leaves it alone.

    Returns True if it was turned off now, False if already off / left alone,
    None if the backend has no such setting or the write failed."""
    if str(cfg.get("auto_standby", "off")).lower() == "keep":
        return False
    if "auto_standby" not in {e.key for e in backend.extras()}:
        return None
    cur = backend.get_extra("auto_standby")
    if cur is False:
        return False
    if backend.set_extra("auto_standby", False):
        log.warning("receiver auto-standby was %s -> turned OFF (power policy owns power; "
                    "set auto_standby = keep to leave it)", "ON" if cur else "unknown")
        return True
    log.warning("could not turn off receiver auto-standby; it may power off on its own")
    return None


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

    from .mqtt.paho_client import PahoTransport   # lazy: paho only needed at runtime
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
        own_receiver_power(backend, cfg)

    # Track the last-seen active state per demand so we act only on activation
    # *edges*. This lets a client (e.g. the CEC relay) re-assert its demand
    # periodically -- so the state survives a daemon restart -- without those
    # re-assertions re-selecting the input every time (which would spam the
    # receiver and fight another active source).
    _demand_active: dict[str, bool] = {}
    _demand_input: dict[str, str] = {}
    # When several demands want different inputs, this order decides which one to
    # fall back to when a source stops but the receiver stays on (e.g. AirPlay
    # stops while the TV is still on -> go back to the TV input). Earlier = wins.
    _demand_priority = ["cec"]

    def feed_demand(name, active, inp=None):
        if policy is None:
            return
        with lock:
            was = _demand_active.get(name, False)
            edge_on = active and not was
            edge_off = was and not active
            _demand_active[name] = active
            act = policy.set_demand(name, active, time.monotonic())
            chosen = inp or (airplay_input if name.startswith("airplay") else "")
            if active and chosen:
                _demand_input[name] = chosen
            if act is True:
                # Power transitioned on (fresh daemon or was off): power + input.
                backend.set_power(True)
                if chosen:
                    backend.set_input(chosen)
                log.info("power: ON (demand %s)%s", name,
                         f" + input {chosen}" if chosen else "")
            elif act is False:
                backend.set_power(False)
                log.info("power: OFF (no demand, grace elapsed)")
            elif edge_on and chosen and backend.get_power():
                # Newly-activated source while already on -> switch to its input.
                # (A periodic re-assertion is active-but-not-an-edge: do nothing.)
                backend.set_input(chosen)
                log.info("input -> %s (demand %s active)", chosen, name)
            elif edge_off and backend.get_power():
                # A source stopped but the receiver stays on (another demand still
                # holds it, e.g. the TV). Restore the input of a remaining demand
                # so we don't sit on the now-silent input (AirPlay -> back to TV).
                remaining = [n for n, a in _demand_active.items()
                             if a and n != name and _demand_input.get(n)]
                remaining.sort(key=lambda n: _demand_priority.index(n)
                               if n in _demand_priority else len(_demand_priority))
                if remaining:
                    tgt = _demand_input[remaining[0]]
                    backend.set_input(tgt)
                    log.info("input -> %s (demand %s cleared, %s still active)",
                             tgt, name, remaining[0])

    # ---- optional AirPlay volume sync (phone slider -> receiver) ---------
    airplay_min_db = float(cfg.get("airplay_min_db", "-30"))
    # airplay_curve: "flat" (linear in dB) or "standard" (shairport-sync's own
    # mixer taper; use it for a receiver that used to be driven through the
    # ALSA mixer plugins so the phone slider keeps the feel it had).
    airplay_curve = cfg.get("airplay_curve", "flat").strip().lower()
    if airplay_curve not in AIRPLAY_CURVES:
        raise SystemExit(f"config: airplay_curve must be one of {AIRPLAY_CURVES}, "
                         f"got {airplay_curve!r}")
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
            pct = clamp_absolute(
                airplay_db_to_pct(db, min_db=airplay_min_db, curve=airplay_curve), ceiling)
            log.info("airplay volume: %.1f dB -> %d (%s curve, ceiling %d)",
                     db, pct, airplay_curve, ceiling)
            backend.set_volume(pct)

    bridge = MqttBridge(device, backend, transport,
                        ceiling=ceiling,
                        discovery_prefix=cfg.get("discovery_prefix", "homeassistant"),
                        on_demand=(feed_demand if policy is not None else None),
                        lock=lock)

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
    # Per-zone context for the poll loop: (zone_target, watcher, zone_policy).
    # zone_policy is a PowerPolicy for extra zones (grace before power-off, so a
    # brief pause/track-gap doesn't power-cycle the zone); None for the main zone
    # (it uses the shared power policy) and when there's no power policy at all.
    zone_ctx: list[tuple] = []
    watcher = None
    zone_off_grace = float(cfg.get("zone_off_grace", "45"))
    zones = parse_zones(cfg)
    if zones:
        for z in zones:
            zceil = clamp_ceiling(int(z["max_volume"])) if z["max_volume"] else ceiling
            zt = make_zone_target(z["target"], backend, name=z["name"],
                                  ceiling=zceil, source=z["input"],
                                  airplay_min_db=airplay_min_db,
                                  airplay_curve=airplay_curve)
            w = ShairportWatcher(bus=z["bus"], label=z["name"], lock=lock)
            zpol = None
            if z["target"] == "main" and policy is not None:
                # Main routes power through the shared policy (so the CEC/TV demand
                # and AirPlay demand cooperate).
                w.on_playing = (lambda p, zn=z["name"], inp=z["input"]:
                                feed_demand(f"airplay:{zn}", p, inp))
            elif policy is not None:
                # Extra zone: own power, but debounced through its own policy so a
                # momentary not-playing doesn't flap the zone off.
                zpol = PowerPolicy(off_grace_s=zone_off_grace)

                def _zpower(active, zt=zt, zpol=zpol):
                    cmd = zpol.set_demand("play", active, time.monotonic())
                    if cmd is True:
                        zt.on_playing(True)          # power on + select source
                    elif cmd is False:
                        zt.set_power(False)
                        log.info("zone %s: power OFF (grace elapsed)", zt.name)
                w.on_playing = _zpower
            else:
                w.on_playing = zt.on_playing
            # Volume sync (phone slider -> receiver zone volume, clamped to the
            # zone ceiling). volume=off leaves the zone's volume untouched (the
            # endpoint does its own software volume).
            if truthy(z.get("volume", "on")):
                w.on_volume = (lambda db, zt=zt, w=w: zt.on_volume(db, w.state.playing()))
            watchers.append(w)
            zone_ctx.append((zt, w, zpol))
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
             "power_policy=%s shairport=%s airplay_curve=%s", backend.name, node_id,
             transport.host, transport.port, ceiling, socket_path if srv_ok else "(none)",
             policy is not None, watcher is not None, airplay_curve)
    transport.start()
    try:
        while not stop.wait(poll_interval):
            # Keep each lock hold short so interactive socket commands (CEC/phone
            # volume, DEMAND) never wait behind a full serial state sweep.
            with lock:
                if not backend.is_connected():
                    backend.connect()
                ev = backend.poll_events()
                if not ev.empty():
                    log.info("receiver reported: %s", ev.describe())
                if policy is not None:
                    # The receiver can change power on its own (remote, front
                    # panel, its own timer); keep the policy's idea of ownership
                    # in step so a later demand edge still powers it on.
                    actual = ev.power if ev.power is not None else backend.get_power()
                    seen = policy.observe_power(actual)
                    if seen == "external_off":
                        log.warning("power: receiver went OFF on its own (not commanded); "
                                    "demands %s -- will power on again on the next demand",
                                    sorted(policy.demands()) or "none")
                    elif seen == "external_on":
                        log.info("power: receiver turned ON externally%s",
                                 " (adopted: a demand is active)" if policy.demands() else
                                 " (not ours; will not power it off)")
                    if policy.tick(time.monotonic()) is False:
                        backend.set_power(False)
                        log.info("power: OFF (no demand, grace elapsed)")
            # Per-zone: flush matured power-off grace, then reverse volume sync
            # (receiver remote/front panel -> phone slider). One lock per zone.
            now = time.monotonic()
            for zt, w, zpol in zone_ctx:
                connected = w.state.active or w.state.playing()
                with lock:
                    if zpol is not None and zpol.tick(now) is False:
                        zt.set_power(False)
                        log.info("zone %s: power OFF (grace elapsed)", zt.name)
                    # If the receiver's input was switched away from this zone's
                    # AirPlay input, end the session so the phone stops streaming
                    # into a source that isn't playing.
                    zt.check_input_stolen(connected, w.drop_session, now)
            # publish_state takes the lock per read internally (released between
            # reads), so this long sweep doesn't block interactive commands.
            bridge.publish_state()
    finally:
        for w in watchers:
            w.stop()
        if srv_ok:
            srv.stop()
        with lock:
            bridge.set_availability(False)
            bridge.set_daemon_online(False)
        transport.stop()
        backend.close()
    return 0


def _locked(lock, fn):
    with lock:
        fn()


if __name__ == "__main__":
    raise SystemExit(main())

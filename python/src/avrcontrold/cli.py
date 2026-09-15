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
from .protocol import avr_socket_path
from .socket_server import SocketServer

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
    bridge = MqttBridge(device, backend, transport,
                        ceiling=ceiling,
                        discovery_prefix=cfg.get("discovery_prefix", "homeassistant"))

    lock = threading.Lock()
    transport.on_connect = lambda: _locked(lock, bridge.announce)
    transport.on_message = lambda t, p: _locked(lock, lambda: bridge.handle_message(t, p))

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    # Unix-socket server so the ALSA plugins and avr-cec-relay bind to this
    # daemon. Path matches the C++/relay convention so existing clients find it.
    kind = cfg.get("backend", "nad").lower()
    stype = cfg.get("socket_type") or _SOCKET_TYPE.get(kind, kind)
    socket_path = cfg.get("avr_socket") or avr_socket_path(stype, cfg)
    srv = SocketServer(socket_path, backend, lock, ceiling, on_shutdown=stop.set)
    srv_ok = srv.start_listening()
    if srv_ok:
        srv.start()
    else:
        log.warning("socket server disabled (bind failed on %s)", socket_path)

    log.info("starting: backend=%s node_id=%s mqtt=%s:%s ceiling=%s socket=%s",
             backend.name, node_id, transport.host, transport.port, ceiling,
             socket_path if srv_ok else "(none)")
    transport.start()
    try:
        while not stop.wait(poll_interval):
            with lock:
                if not backend.is_connected():
                    backend.connect()
                ev = backend.poll_events()
                if not ev.empty():
                    log.debug("push event: %s", ev)
                bridge.publish_state()
    finally:
        if srv_ok:
            srv.stop()
        with lock:
            bridge.set_availability(False)
        transport.stop()
        backend.close()
    return 0


def _locked(lock: threading.Lock, fn):
    with lock:
        fn()


if __name__ == "__main__":
    raise SystemExit(main())

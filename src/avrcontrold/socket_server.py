"""Unix-socket server exposing the receiver over the text protocol.

Runs in its own thread; backend access is serialised through a shared lock so it
is safe alongside the MQTT bridge and the poll loop. Clients (ALSA plugins,
avr-cec-relay) connect, send one command line, and get one reply line.
"""

from __future__ import annotations

import logging
import os
import socket
import threading

from .backends.base import Backend
from .protocol import dispatch

log = logging.getLogger(__name__)


class SocketServer(threading.Thread):
    def __init__(self, path: str, backend: Backend, lock: threading.Lock,
                 vol_ceiling: int, *, meta_provider=None, playstate_provider=None,
                 demand_handler=None, on_shutdown=None):
        super().__init__(name="avr-socket", daemon=True)
        self.path = path
        self.backend = backend
        self.lock = lock
        self.vol_ceiling = vol_ceiling
        self.meta_provider = meta_provider
        self.playstate_provider = playstate_provider
        self.demand_handler = demand_handler
        self.on_shutdown = on_shutdown
        self._stop = threading.Event()
        self._sock: socket.socket | None = None

    def start_listening(self) -> bool:
        try:
            os.makedirs(os.path.dirname(self.path), mode=0o777, exist_ok=True)
        except OSError:
            pass
        try:
            if os.path.exists(self.path):
                os.unlink(self.path)
        except OSError:
            pass
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.bind(self.path)
        except OSError as exc:
            log.error("socket: bind %s failed: %s", self.path, exc)
            s.close()
            return False
        try:
            os.chmod(self.path, 0o666)   # cross-user clients (root/shairport)
        except OSError:
            pass
        s.listen(16)
        s.settimeout(0.5)
        self._sock = s
        log.info("socket: listening on %s (ceiling=%d)", self.path, self.vol_ceiling)
        return True

    def run(self) -> None:
        if self._sock is None and not self.start_listening():
            return
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()
        if self._sock:
            self._sock.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def _serve(self, conn: socket.socket) -> None:
        conn.settimeout(30)
        buf = b""
        with conn:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(1024)
                except (socket.timeout, OSError):
                    return
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if not line:
                        continue
                    if line == "SHUTDOWN":
                        try:
                            conn.sendall(b"OK\n")
                        except OSError:
                            pass
                        if self.on_shutdown:
                            self.on_shutdown()
                        return
                    with self.lock:
                        reply = dispatch(self.backend, line, self.vol_ceiling,
                                         self.meta_provider, self.playstate_provider,
                                         self.demand_handler)
                    try:
                        conn.sendall((reply + "\n").encode())
                    except OSError:
                        return

    def stop(self) -> None:
        self._stop.set()

"""shairport-sync D-Bus integration (play-state, now-playing, AirPlay volume).

Split in two:

* ``ShairportState`` -- pure, dependency-free state + the logic that folds a
  ``PropertiesChanged`` body into it. Fully unit-testable with plain dicts in
  jeepney's variant form ``{key: (signature, value)}``.
* ``ShairportWatcher`` -- a jeepney (pure-Python D-Bus) thread that connects to
  the system bus, reads the initial state, subscribes to PropertiesChanged, and
  drives ``ShairportState`` + callbacks. jeepney keeps the venv arch-independent.

Interfaces (same as the C++ shairport_dbus.cpp):
  org.gnome.ShairportSync                -> Active (b)
  org.gnome.ShairportSync.RemoteControl  -> PlayerState (s), AirplayVolume (d),
                                            Metadata (a{sv})
"""

from __future__ import annotations

import logging
import threading

log = logging.getLogger(__name__)

BUS_NAME = "org.gnome.ShairportSync"
PATH = "/org/gnome/ShairportSync"
IFACE_MAIN = "org.gnome.ShairportSync"
IFACE_REMOTE = "org.gnome.ShairportSync.RemoteControl"


def _unwrap(v):
    """jeepney represents a variant as (signature, value); unwrap to the value."""
    if isinstance(v, tuple) and len(v) == 2 and isinstance(v[0], str):
        return v[1]
    return v


class ShairportState:
    def __init__(self):
        self.active: bool = False
        self.player_state: str = ""
        self.title: str = ""
        self.artist: str = ""
        self.album: str = ""
        self.volume_db: float | None = None

    def playing(self) -> bool:
        return self.player_state == "Playing"

    def apply_metadata(self, meta: dict) -> None:
        # meta is a{sv}: {key: (sig, value)}
        def g(key):
            return _unwrap(meta[key]) if key in meta else None
        t = g("xesam:title")
        al = g("xesam:album")
        ar = g("xesam:artist")   # array of strings
        if t is not None:
            self.title = str(t)
        if al is not None:
            self.album = str(al)
        if ar is not None:
            if isinstance(ar, (list, tuple)):
                self.artist = str(ar[0]) if ar else ""
            else:
                self.artist = str(ar)

    def apply_changed(self, interface: str, changed: dict) -> set[str]:
        """Fold a PropertiesChanged body in. Returns the set of high-level fields
        that changed: any of {'playing','volume','metadata','active'}."""
        touched: set[str] = set()
        if interface == IFACE_MAIN:
            if "Active" in changed:
                self.active = bool(_unwrap(changed["Active"]))
                touched.add("active")
        elif interface == IFACE_REMOTE:
            if "PlayerState" in changed:
                new = str(_unwrap(changed["PlayerState"]) or "")
                if new != self.player_state:
                    self.player_state = new
                    touched.add("playing")
            if "AirplayVolume" in changed:
                self.volume_db = float(_unwrap(changed["AirplayVolume"]))
                touched.add("volume")
            if "Metadata" in changed:
                self.apply_metadata(_unwrap(changed["Metadata"]) or {})
                touched.add("metadata")
        return touched

    def _san(self, s: str) -> str:
        return s.replace("|", " ").replace("\n", " ").replace("\r", " ")

    def snapshot(self) -> str:
        return ("title=" + self._san(self.title) +
                "|artist=" + self._san(self.artist) +
                "|album=" + self._san(self.album) +
                "|state=" + (self.player_state or "Unknown") +
                "|active=" + ("1" if self.active else "0"))


class ShairportWatcher(threading.Thread):
    """Watches shairport-sync over the system bus. Best-effort: if D-Bus or
    shairport is unavailable, it logs and the daemon runs without it."""

    def __init__(self, *, on_playing=None, on_change=None, on_volume=None,
                 lock: threading.Lock | None = None,
                 bus: str = "SYSTEM", label: str | None = None):
        super().__init__(name=f"shairport-dbus[{label or bus}]", daemon=True)
        self.state = ShairportState()
        self.on_playing = on_playing   # called(bool) when play-state flips
        self.on_change = on_change     # called() on any metadata/state change
        self.on_volume = on_volume     # called(db: float) when AirplayVolume changes
        self.lock = lock or threading.Lock()
        # Bus to connect to: "SYSTEM"/"SESSION", or a literal D-Bus address such
        # as "unix:path=/run/pi-audio/livingroom/bus". A per-zone private bus lets
        # several shairport instances (each in its own Docker container) coexist
        # despite the fixed well-known name org.gnome.ShairportSync.
        self.bus = bus
        self.label = label or bus
        self._stop = threading.Event()
        self._conn = None
        self._writer = None            # lazy, separate connection for writes

    # ---- I/O (jeepney) --------------------------------------------------
    def _read_initial(self, conn):
        from jeepney import DBusAddress, MessageType, Properties
        for iface, prop, setter in (
            (IFACE_MAIN, "Active", "Active"),
            (IFACE_REMOTE, "PlayerState", "PlayerState"),
            (IFACE_REMOTE, "AirplayVolume", "AirplayVolume"),
            (IFACE_REMOTE, "Metadata", "Metadata"),
        ):
            try:
                addr = DBusAddress(PATH, bus_name=BUS_NAME, interface=iface)
                reply = conn.send_and_get_reply(Properties(addr).get(prop), timeout=2)
                # An error reply (shairport not running / property absent) comes
                # back as a message, not an exception -- skip it, don't apply its
                # error string as state.
                if reply.header.message_type != MessageType.method_return:
                    log.debug("shairport initial %s.%s unavailable", iface, prop)
                    continue
                self.state.apply_changed(iface, {setter: reply.body[0]})
            except Exception as exc:  # property may be absent / shairport idle
                log.debug("shairport initial %s.%s: %s", iface, prop, exc)

    def run(self) -> None:
        try:
            from jeepney import MatchRule, message_bus
            from jeepney.io.blocking import open_dbus_connection
        except Exception as exc:  # pragma: no cover - import guard
            log.warning("shairport: jeepney unavailable: %s", exc)
            return
        try:
            conn = open_dbus_connection(bus=self.bus)
        except Exception as exc:
            log.warning("shairport[%s]: cannot open bus %s: %s",
                        self.label, self.bus, exc)
            return
        self._conn = conn
        rule = MatchRule(type="signal", interface="org.freedesktop.DBus.Properties",
                         member="PropertiesChanged", path=PATH)
        try:
            conn.send_and_get_reply(message_bus.AddMatch(rule), timeout=2)
        except Exception as exc:
            log.warning("shairport: AddMatch failed: %s", exc)
        with self.lock:
            self._read_initial(conn)
            self._emit_playing(initial=True)
        log.info("shairport[%s]: watching %s on %s (playing=%s)", self.label,
                 BUS_NAME, self.bus, self.state.playing())
        with conn.filter(rule) as q:
            while not self._stop.is_set():
                try:
                    msg = conn.recv_until_filtered(q, timeout=0.5)
                except TimeoutError:
                    continue
                except Exception as exc:
                    log.debug("shairport: recv error: %s", exc)
                    continue
                if msg is None or len(msg.body) < 2:
                    continue
                # Coalesce a burst: dragging the phone slider emits many
                # AirplayVolume signals in a few ms. Drain everything already
                # queued and fold it into state, then act ONCE on the final
                # value -- otherwise every intermediate volume is written to the
                # receiver over the (slow) serial port, so the last one lands
                # seconds late. Non-burst traffic drains empty immediately and
                # behaves as before.
                batch = [msg]
                while True:
                    try:
                        m2 = conn.recv_until_filtered(q, timeout=0.05)
                    except TimeoutError:
                        break
                    except Exception:
                        break
                    if m2 is None or len(m2.body) < 2:
                        continue
                    batch.append(m2)
                with self.lock:
                    touched: set[str] = set()
                    for m in batch:
                        touched |= self.state.apply_changed(m.body[0], m.body[1])
                    if "playing" in touched:
                        self._emit_playing()          # re-applies volume on a play edge
                    elif "volume" in touched and self.on_volume and self.state.volume_db is not None:
                        self.on_volume(self.state.volume_db)
                    if touched and self.on_change:
                        self.on_change()
        try:
            conn.close()
        except Exception:
            pass


    def _emit_playing(self, initial: bool = False) -> None:
        if self.on_playing:
            self.on_playing(self.state.playing())
        # A sender sets its volume BEFORE it starts playing, and the volume
        # handlers ignore volume while idle (an idle instance must never move
        # the receiver). So replay the last known volume on the play edge: the
        # slider level lands on the receiver right at power-on instead of the
        # receiver playing full-scale input at whatever level it remembered.
        # (Seen live: the Opalum stayed at its primed 20 while the sender sat
        # at 15% -> -24 dB, because the volume arrived before PlayerState.)
        if self.state.playing() and self.on_volume and self.state.volume_db is not None:
            self.on_volume(self.state.volume_db)

    # ---- write path (drop the AirPlay session on input steal) -----------
    def drop_session(self) -> bool:
        """Drop the current AirPlay session (org.gnome.ShairportSync.DropSession).

        Used when the receiver's input is switched away from this zone's AirPlay
        input: the stream is no longer audible, so end the session and let the
        sender (phone) fall back to its own output.
        """
        try:
            from jeepney import DBusAddress, new_method_call
            if self._writer is None:
                from jeepney.io.blocking import open_dbus_connection
                self._writer = open_dbus_connection(bus=self.bus)
            addr = DBusAddress(PATH, bus_name=BUS_NAME, interface=IFACE_MAIN)
            self._writer.send_and_get_reply(new_method_call(addr, "DropSession"),
                                            timeout=2)
            log.info("shairport[%s]: dropped AirPlay session (input switched away)",
                     self.label)
            return True
        except Exception as exc:
            log.debug("shairport[%s]: DropSession failed: %s", self.label, exc)
            self._writer = None
            return False

    def stop(self) -> None:
        self._stop.set()
        if self._writer is not None:
            try:
                self._writer.close()
            except Exception:
                pass
            self._writer = None

    # ---- providers for the socket protocol ------------------------------
    def snapshot(self) -> str:
        with self.lock:
            return self.state.snapshot()

    def playstate(self) -> str:
        with self.lock:
            return self.state.player_state or "-"

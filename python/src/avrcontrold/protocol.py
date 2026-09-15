"""The Unix-socket text protocol -- the stable contract with existing clients.

The C ALSA plugins and the avr-cec-relay speak this exact newline-delimited
protocol to whatever daemon owns the receiver. Keeping it identical to the C++
daemon's (include/avr_protocol.hpp + the dispatch in avr_controld.cpp) means
those clients bind to this Python daemon unchanged.

`avr_socket_path` must match the C++ helper byte-for-byte so a client and the
daemon derive the same path: sanitize replaces each non-alphanumeric character
with '_' (per character, no run-collapsing), identity is the device path if
present else host:port.
"""

from __future__ import annotations

import os

from .backends.base import Backend
from .volume import clamp_absolute


# ---- socket path (must match the C++ avr_protocol.hpp) ---------------------
def avr_socket_dir() -> str:
    d = os.environ.get("AVR_SOCKET_DIR") or "/run/avr-controld"
    try:
        os.makedirs(d, mode=0o777, exist_ok=True)
    except OSError:
        pass
    return d


def avr_sanitize(s: str) -> str:
    return "".join(c if c.isalnum() and c.isascii() else "_" for c in s)


def avr_identity(cfg: dict) -> str:
    dev = cfg.get("device")
    if dev:
        return dev
    return f"{cfg.get('host', '')}:{cfg.get('port', '')}"


def avr_socket_path(backend_type: str, cfg: dict) -> str:
    return (avr_socket_dir() + "/" + avr_sanitize(backend_type) + "_"
            + avr_sanitize(avr_identity(cfg)) + ".sock")


# ---- dispatch --------------------------------------------------------------
def _opt_int(v: int | None) -> str:
    return str(v) if v is not None else "-"


def _opt_bool(v: bool | None) -> str:
    return ("1" if v else "0") if v is not None else "-"


def _opt_str(v: str | None) -> str:
    return v if v is not None else "-"


def dispatch(backend: Backend, line: str, vol_ceiling: int,
             meta_provider=None, playstate_provider=None, demand_handler=None) -> str:
    """Handle one protocol line, return the single reply line (no newline).

    vol_ceiling is the HARD cap on any absolute volume this daemon will set --
    the last-line-of-defence clamp, mirroring the C++ daemon. 100 = no cap.
    """
    parts = line.split()
    if not parts:
        return "ERR empty"
    verb = parts[0]

    def boolr(ok: bool) -> str:
        return "OK" if ok else "ERR failed"

    if verb == "PING":
        return "OK"
    if verb == "NAME":
        return "OK " + backend.name
    if verb == "CONNECTED":
        return "OK " + ("1" if backend.is_connected() else "0")

    if verb == "CAPS":
        c = backend.capabilities()
        return ("OK abs=%d query=%d mute=%d input=%d spk=%d standby=%d minv=%d maxv=%d"
                % (c.absolute, c.query, c.mute, c.input_select, c.speaker_level,
                   c.standby_timeout, c.min_volume, c.max_volume))
    if verb == "HWMAX":
        return "OK " + str(backend.capabilities().max_volume or 98)
    if verb == "SPKMIN":
        return "OK 38"
    if verb == "SPKMAX":
        return "OK 62"

    if verb == "VOLUP":
        return boolr(backend.volume_up())
    if verb == "VOLDOWN":
        return boolr(backend.volume_down())

    if verb == "INPUTS":
        return "OK " + ",".join(backend.get_inputs())
    if verb == "SPEAKERS":
        return "OK "

    if verb == "DEMAND":
        # DEMAND <name> <0|1> [input...] -- feed the power policy. A client on the
        # CEC bus (avr-cec-relay) uses this to say "the setup needs the receiver"
        # (with the input to select) or no longer does. No-op if no policy.
        if demand_handler is None:
            return "OK -"
        if len(parts) < 3:
            return "ERR bad DEMAND"
        name = parts[1]
        active = parts[2] not in ("0", "off", "false")
        toks = line.split(None, 3)
        inp = toks[3] if active and len(toks) > 3 else None
        demand_handler(name, active, inp)
        return "OK"

    if verb == "META":
        return "OK " + (meta_provider() if meta_provider else "-")
    if verb == "PLAYSTATE":
        return "OK " + (playstate_provider() if playstate_provider else "-")

    if verb == "GET":
        noun = parts[1] if len(parts) > 1 else ""
        if noun == "volume":
            return "OK " + _opt_int(backend.get_volume())
        if noun == "mute":
            return "OK " + _opt_bool(backend.get_mute())
        if noun == "power":
            return "OK " + _opt_bool(backend.get_power())
        if noun == "input":
            return "OK " + _opt_str(backend.get_input())
        return "ERR bad GET"

    if verb == "SET":
        noun = parts[1] if len(parts) > 1 else ""
        if noun == "volume":
            if len(parts) < 3:
                return "ERR bad value"
            try:
                v = int(parts[2])
            except ValueError:
                return "ERR bad value"
            return boolr(backend.set_volume(clamp_absolute(v, vol_ceiling)))
        if noun == "mute":
            if len(parts) < 3:
                return "ERR bad value"
            return boolr(backend.set_mute(parts[2] != "0"))
        if noun == "power":
            if len(parts) < 3:
                return "ERR bad value"
            return boolr(backend.set_power(parts[2] != "0"))
        if noun == "input":
            if len(parts) < 3:
                return "ERR bad value"
            name = line.split(None, 2)[2]
            return boolr(backend.set_input(name))
        return "ERR bad SET"

    return "ERR unknown verb"

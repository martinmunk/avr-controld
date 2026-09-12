# avr-controld: single-owner connection broker

## Problem
The AVR connection (UART serial or TCP) is opened per-process. shairport's mixer,
the `avr-power` session-hook processes, and any `amixer` each open their own
connection. The `static` connection pools only dedup *within* a process. For
serial this means multiple processes share one wire with no locking (garbled
commands); for TCP it means connection churn / rapid-reconnect refusals and no
shared state.

## Approach
A small daemon, `avr-controld`, owns exactly one connection per physical AVR and
serves a local Unix socket. The ALSA plugin's backend becomes a thin socket
client (`SocketClientBackend`). Because `avr-power` and `amixer` also go through
the ALSA plugin, *every* opener automatically becomes a client of the one daemon.
The daemon serializes all access and holds the authoritative state.

    shairport (mixer plugin)  \
    avr-power (hook)           >-- SocketClientBackend --> /run/avr-controld/<id>.sock --> avr-controld --> real Backend --> AVR
    amixer -D denon:...       /

## Pieces
- `include/avr_protocol.hpp` - newline-delimited text protocol + deterministic
  socket-path derivation (shared by daemon and client).
- `src/daemon/avr_controld.cpp` - parses `--type` + `-o key=value` config,
  creates the real backend via `BackendFactory`, `init()`s it, serves the socket
  (single-threaded poll loop, so access is naturally serialized). One daemon per
  physical device, guarded by an flock lockfile taken BEFORE init so a losing
  race never opens the device.
- `src/backends/socket_client.cpp` - `SocketClientBackend` implements `Backend`
  by forwarding each call over the socket. On first use it connects; if no daemon
  is listening it auto-spawns one (fork+exec; the daemon self-daemonizes) and
  waits for the socket. No systemd dependency required. It also provides the
  `create_<type>_backend()` symbols the plugins already call, so the plugins and
  avr-power need NO code change - only the build links this instead of the real
  backend.
- `systemd/tmpfiles.d/avr-controld.conf` - owns `/run/avr-controld` (0777) so the
  cross-user auto-spawn can always create/find sockets there.

## Socket-path identity
`/run/avr-controld/<type>_<id>.sock` where `<id>` = sanitized `config["device"]`
if present, else `config["host"]:config["port"]`. This keys the daemon to the
physical device: differing `log_*`/`input` cosmetic config still shares one
daemon. Socket mode 0666 for cross-user (shairport-sync/root) access.

## Protocol (text, newline-terminated; reply `OK[ payload]` or `ERR msg`)
`PING`, `CAPS`, `HWMAX`, `NAME`, `CONNECTED`,
`GET volume|mute|power|standby|input`, `SET volume|mute|power|standby|input <v>`,
`VOLUP`, `VOLDOWN`, `INPUTS`, `SPEAKERS`, `GET speaker <S>`, `SET speaker <S> <v>`,
`SPKMIN`, `SPKMAX`. Unknown/`nullopt` state is returned as payload `-`.

## Status
Daemon protocol validated against the live Denon (caps, hwmax=70, volume, inputs,
11 speakers). Builds clean (daemon + 4 plugins + avr-power). Remaining: on-device
end-to-end test (plugin -> client -> daemon -> AVR) after install, and an optional
explicit systemd unit (auto-spawn already covers lifecycle).

## Session loss and live state (0.3.1)

The receiver drops idle telnet sessions (standby, network hiccups). On 0.3.0 the
daemon kept the dead socket forever: `send()` failed, nothing marked the
connection down, and `reconnect()` was a no-op - so StueTaerten had no volume
or input control for days. Now:

* `DenonConnection` checks for the receiver's close before every send (a
  zero-timeout read - after a FIN a `send()` still "succeeds" locally and the
  command would be lost), drops the socket on EOF/error, reconnects once and
  re-sends. A reconnect re-probes all state.
* `get_volume/mute/power/input` ask the receiver every time (`MV?`, `MU?`,
  `PW?`, `SI?`) and fall back to the cache only if it does not answer. The
  receiver is also driven by its remote and by HDMI-CEC from the TV, so a
  cache in a weeks-old daemon is wrong. `get_power` used to return
  `is_connected()`, which is why `avr-power restore` always said "power was
  already on".
* The `input` parameter is no longer applied by the backend at init (once per
  daemon lifetime is meaningless). It is passed through to clients as
  `Backend::configured_input()`, exposed by the plugins as the read-only
  `Input Source Default` element, and applied per session by `avr-power on`.
* Mock hooks `XDROP` / `XDROPOTHERS` (not Denon protocol) let the integration
  and broker tests kill the session under test.

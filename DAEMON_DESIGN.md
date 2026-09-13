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

## shairport-sync D-Bus integration (0.3.1, optional)

`avr-controld` can watch shairport-sync directly over D-Bus and orchestrate AVR
power/input from real playback state, closing the loop the coarse
`run_this_*_active_state` shell hooks cannot: the hooks fire `PWON` blindly and
never confirm the receiver obeyed. This is **additive and off by default** --
volume and mute stay entirely on the ALSA hardware-mixer path (hardware dB
attenuation, mute-on-pause), which other consumers (amixer, avr-power, any
renderer) rely on. D-Bus is only for events, metadata, and power/input.

### Why sd-bus
libsystemd is installed on every Debian 13 node (systemd is PID 1) and
cross-installs as `libsystemd-dev:<arch>`, so it adds no real runtime
dependency (`libsystemd0` is already present). It exposes one pollable fd
(`sd_bus_get_fd`) that folds directly into the daemon's existing single poll()
loop -- no glib main loop, no second thread. The bus fd is appended to the
pollfd set each iteration and serviced before clients; it is never treated as a
client socket.

### D-Bus surface consumed (shairport-sync 5.2.1, system bus)
* name `org.gnome.ShairportSync`, path `/org/gnome/ShairportSync`
* `org.gnome.ShairportSync` prop `Active` (b) -- a client session is up
* `org.gnome.ShairportSync.RemoteControl` props `PlayerState` (s: Playing/
  Paused/Stopped), `Metadata` (a{sv}, xesam:title/artist/album), `ProgressString`
* all delivered via `org.freedesktop.DBus.Properties.PropertiesChanged`; read
  once at startup via `Get` so we are not blind until the first change.

### Decision logic (SessionPolicy, pure + unit-tested)
`include/avr_session_policy.hpp` turns the signal stream into AVR actions with
no D-Bus/Backend deps (so it is unit-tested in isolation, tests/unit/
test_session_policy.cpp):
* PlayerState -> Playing: power on + select configured input, **once** (repeated
  Playing signals are debounced, no re-thrash).
* Paused / Stopped / session gone: arm a grace timer (default 120s), do not
  power off yet -- a brief pause or track gap must not cycle the AVR.
* grace elapses with no resume: power off. Resume before it cancels the timer.
* session up alone (connected, not playing): nothing, so merely selecting the
  device never powers the AVR on.

### Verify-and-retry
In `drive` mode the daemon does not trust a fire-and-forget `PWON`. After
setting power+input it reads the receiver back (the live reads added earlier)
and retries up to 4 times; the backend's own reconnect/backoff absorbs the
~1s window where the receiver refuses telnet during power-on. Power-off never
switches the input (an HDMI input on the way to standby is what drives the
Denon/TV/Shield CEC wake cascade).

### Configuration
`dbus = off | observe | drive` (default `off`):
* `off` -- no D-Bus; daemon behaves exactly as before.
* `observe` -- connect, log play/pause + now-playing, never touch the AVR.
* `drive` -- also power the AVR on/off with verification as above.
`input` -- the input to select on play (already passed to the daemon).
`dbus_off_grace` -- seconds to wait after stop before powering off (default 120).

Because the daemon is normally auto-spawned by the ALSA client (whose args come
from the asound conf and carry no `dbus=`), enable the feature per node by
dropping keys in **/etc/avr-controld.conf** (`key=value` lines), which the
daemon merges at startup for keys not already on the command line, e.g.:
```
dbus=drive
dbus_off_grace=180
```
This file is host-global: on a node with several AVRs it applies to each
daemon (these single-AVR Pis are the intended case). When `dbus=drive` is
enabled, remove the `run_this_*_active_state` avr-power hooks from
shairport-sync.conf so the two do not both drive power.

Alternatively run the daemon from a systemd unit with explicit `-o dbus=drive`;
the per-device flock means that daemon owns the AVR and the ALSA clients simply
connect to it.

### New socket verbs
* `META` -> `OK title=..|artist=..|album=..|state=..|active=0/1|progress=..`
* `PLAYSTATE` -> `OK Playing|Paused|Stopped|-`
so a future display/MQTT consumer can read now-playing from the daemon. Both
return `OK -` when D-Bus is off.

### Test hook
`AVR_DBUS_ADDRESS` makes the watcher open a given bus address instead of the
system bus, so a fake shairport on a private `dbus-run-session` bus can drive it.
The play/pause->power state machine has full unit coverage; the sd-bus decode
and the verify-retry orchestration were validated live against the real
shairport-sync 5.2.1 + Denon on StueTaerten (observe mode: correct decode of
now-playing/PlayerState/Active and the META/PLAYSTATE verbs, AVR untouched).

## Volume sync (shairport `.Volume` <-> AVR volume/mute, bidirectional)

`volume_sync = on` (default `off`) makes the daemon the single hub that keeps the
AirPlay volume and the receiver's volume/mute in step in BOTH directions, while
the audio stays full-range digital (the receiver does the attenuation).

### Required shairport settings (BOTH mandatory)
1. `ignore_volume_control = "yes"` in `general {}` -- shairport outputs full-scale
   and does not attenuate; the receiver, driven by the daemon, does the attenuation.
2. **Remove `mixer_control_name` / `mixer_device` from `alsa {}`.** This is the one
   that bit us: under `ignore_volume_control=yes` shairport does NOT leave the mixer
   alone -- it drives the configured mixer to MAXIMUM, which reaches the receiver
   through the ALSA plugin as an uncapped `SET volume 100` and overrides the
   daemon's cap (the full-volume incident). With no mixer configured, shairport
   touches no hardware and only the daemon sets the receiver's volume.

shairport still tracks the AirPlay volume and exposes it, writable, on D-Bus
(`org.gnome.ShairportSync` `.Volume`, dB, `-144`=mute). `avr-power` keeps its own
control handle for power/input, so removing the volume mixer does not affect it.

### Flow
* **Phone -> AVR:** the watcher's `Volume` handler feeds `VolumeSync::on_airplay_db`.
  AirPlay dB (`-30..0`, `-144`=mute) maps to 0-100 percent (see `volume_map.hpp`),
  clamped to `max_volume`, and is sent to the receiver -- coalesced and
  rate-limited (`volume_coalesce_ms`, default 150 ms) so a fast slider drag
  chases the latest target instead of queuing stale `MV` commands.
* **AVR -> phone:** the Denon telnet socket is folded into the poll loop
  (`Backend::event_fd()`); when it is readable the daemon calls
  `Backend::pump_events()`, which classifies the receiver's unsolicited
  `MVxx`/`MUON`/`MUOFF` pushes (from its remote, HDMI-CEC or another controller)
  and reports the change. The daemon maps it back to dB and writes shairport's
  `.Volume`, which shairport reports to the sender so the phone slider follows.
* **Echo suppression:** `VolumeSync` remembers each value it pushes to a side and
  consumes the one matching echo that comes back, so a change is never bounced to
  the side it came from (this is what would otherwise oscillate / feel jerky).

### Safety
`max_volume` (0-100, default 40) is a HARD ceiling enforced on EVERY volume-set
path in the daemon -- both the `VolumeSync` percent and the raw `SET volume` verb
in `dispatch()` are clamped to it -- so a phone at full slider, a wrong value, or
a stray command can never drive the AVR past it. (The earlier design only capped
the `VolumeSync` path, and shairport's mixer drove an uncapped `SET volume 100`
straight past it; hence rule 2 above plus this belt-and-braces clamp.) On playback
start the daemon snaps the receiver to the phone's current volume (capped) so the
first note is at a safe, known level.

### Async design
Still a single-threaded `poll()` loop: the sd-bus fd and the Denon telnet fd are
both folded into it (plus a timer for the coalesce flush). No boost-asio and no
new thread -- boost would be a heavy dependency for a single-connection daemon,
and sd-bus already integrates with the existing loop; sd-event (libsystemd) would
be the natural choice only if this grew much larger.

### Config keys
`volume_sync` (off|on), `max_volume` (0-100, default 40),
`volume_coalesce_ms` (default 150), `airplay_min_db` (default -30.0). Read from
`-o key=value` or `/etc/avr-controld.conf`. `volume_sync=on` implies a D-Bus
connection; if the bus is unavailable the daemon logs and runs ALSA-only.

### Testing status
`volume_map` and the `VolumeSync` echo-suppression/coalescing state machine have
full unit coverage; the `event_fd()`/`pump_events()` push path and no-corruption-
after-push are covered by integration tests against the mock (which grows
`XPUSHMV`/`XPUSHMU` hooks to inject an external change). End-to-end with a real
phone is NOT yet confirmed, because pyatv cannot open a streaming session against
this shairport AP2 build; the D-Bus write path was verified live (writing
`.Volume` round-trips and, with `ignore_volume_control=yes`, does not move the
Denon).

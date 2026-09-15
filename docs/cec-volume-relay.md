# HDMI-CEC volume relay to the NAD (Toslink audio + CEC volume over RS232)

**Status: PROVEN end-to-end on StueTærten (2026-09-15).**

## The question

The NAD T757's HDMI board is dead (lightning). Audio still reaches it over
Toslink. Can we drive the NAD's volume from the TV/Shield remotes by having the
Raspberry Pi sit on the TV's HDMI-CEC bus, receive volume key presses, and relay
them to the NAD over RS232 — with no HDMI audio through the NAD at all?

**Yes.** Audio path and control path are fully separated:

```
 TV / Shield  --Toslink-->  NAD  (audio)
 TV / Shield  --HDMI-CEC-->  Pi (LA5 audio system)  --RS232-->  NAD  (volume)
```

## Why it works (and why my first attempt saw nothing)

The Pi presents itself on the CEC bus as an **Audio System** (logical address 5).
The first attempts failed because the adapter advertised **no device features**
("Device Features: None"), so the TV never treated it as a real audio sink and
sent nothing. Two things fix that:

1. Advertise the **ARC-Rx** feature (`Source Supports ARC Rx`). Now the TV sees a
   genuine audio system on its ARC port.
2. **Initiate ARC** (`<Initiate ARC>` → TV replies `<Report ARC Initiated>`). ARC
   goes Active.

Once that is done, sources on the bus send `<User Control Pressed>` with
`ui-cmd = volume-up / volume-down / mute` addressed straight to LA5. The Pi
translates each into a command to `avr-controld`, which owns the NAD's RS232
connection.

### Roku quirks (confirmed)

- The Roku TV **FEATURE_ABORTs** `<System Audio Mode Request>` (0x70). The formal
  "System Audio Mode" handshake is not supported and is **not needed** — do not
  rely on it. The TV menu showing "System audio control: Inactive" is a red
  herring; volume still flows.
- The **Nvidia Shield** (a playback device on the bus) sends its volume keys
  **directly to LA5**, independent of the TV. This is the proven path.
- **Unverified:** whether the Roku's *own* remote forwards volume to LA5 when a
  Roku app (not the Shield) is the active source. Could not be tested remotely
  (Roku ECP was unreachable). Confirm in normal use; if it does not, the Shield
  remote already covers the common case.

## Proof

NAD started at volume 62. Volume keys sent to the Shield via Home Assistant
(`remote.skaermkarsten`):

| Action | NAD volume (read back over RS232) |
|---|---|
| start | 62 |
| 2× volume-down | 60 |
| 2× volume-up | 62 |
| mute | reported muted |

## Implementation

`avr-cec-relay` (`src/tools/avr-cec-relay.cpp`) — a small daemon:

- Claims LA5 as an Audio System advertising ARC-Rx, using the kernel CEC uAPI
  (`/dev/cec0`). **Gotcha:** `CEC_ADAP_S_LOG_ADDRS` returns `-EBUSY` unless the fd
  is put into `CEC_MODE_INITIATOR` **first** (a freshly opened fd defaults to
  `CEC_MODE_NO_INITIATOR`).
- Initiates ARC and answers the keep-alive messages (`Give Audio Status`,
  `Give System Audio Mode Status`, `Request ARC Initiation`).
- Relays `volume-up`/`volume-down`/`mute` to `avr-controld` over its Unix socket
  (`VOLUP` / `VOLDOWN` / `GET`+`SET mute`).
- **Safety:** every relayed volume-up is gated by a hard `max_volume` ceiling
  (default 80), read live from the receiver, so a stuck key can never walk the
  NAD past the cap.

Config: `/etc/avr-cec-relay.conf` (see `examples/avr-cec-relay.conf`).

## Deployment (StueTærten, done this session)

Built and installed as a tracked package (`0.3.1~dev8`), so `avr-cec-relay` lives
at `/usr/bin` and dpkg owns it:

```bash
TARGET_ARCH=arm64 ./buildpackage.sh          # cross-build here, never on the Pi
sudo dpkg -i martinmunk-alsa-control-avr_*_arm64.deb   # on the Pi
```

`avr-cec-relay.service` is installed and enabled (debhelper enables it on
install); it is harmless if the NAD daemon is absent (it just retries the socket).

### Remaining step for full boot-persistence

The NAD `avr-controld` is currently running ad-hoc:

```bash
avr-controld --type nad_serial -o device=/dev/ttyUSB0 \
             -o log_file=/var/log/alsa-avr/nad.log -o log_level=INFO
```

There is no systemd unit for `avr-controld` (the ALSA plugins auto-spawn it via
`SocketClientBackend`). The relay is **not** an ALSA client and does not spawn it.
So for the relay to work after a reboot, the NAD daemon must be started by
something — either a dedicated unit, or the wider migration of this Pi's
shairport/ALSA output from the Denon to the NAD (which would auto-spawn it). That
migration is a separate decision.

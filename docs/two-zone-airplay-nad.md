# Two-zone AirPlay over a NAD T 757 (Living Room + Dining Room)

Status: design / plan. Not yet built. Supersedes the older, now-stale
`MULTI_ZONE_AND_SHARED_CONNECTION.md` note at the repo root (which predates the
`avr-controld` broker and the D-Bus volume work).

## Goal

Present **two AirPlay endpoints** on the phone — "Living Room" and "Dining
Room" — **each with its own volume slider**, both driving a single **NAD T 757**
receiver that replaces the current living-room Denon:

- **Living Room → NAD Main zone**
- **Dining Room → NAD Zone 2**

The owner is fine with **both rooms playing the same audio**. The only thing that
must be independent per room is **volume** (and on/off), controlled from the
phone's per-speaker AirPlay sliders.

## Hardware reality: the Digi+ is one stream on two connectors

The Raspberry Pi's **HiFiBerry Digi+** exposes an optical (Toslink) **and** a
coaxial (RCA) S/PDIF output, but they are driven **in parallel from one WM8804
S/PDIF transmitter** — they carry the **identical signal simultaneously**. It is
one audio stream on two connectors, **not** two independent outputs. (ALSA
confirms this: the card presents a single playback device, `wm8804-spdif-0`.)

Consequence:

- optical → **NAD Main** input and coax → **NAD Zone 2** input works, but **both
  inputs receive the same audio**.
- Truly **independent content** per room would require a **second real digital
  output** (a USB-to-S/PDIF adapter on the same Pi, or a second Pi). The owner
  accepted same-audio, so this is out of scope for the first build.

Also note: assigning the NAD's surround-back amp channels to **Zone 2** means the
living room (Main) then runs at most **5.1** (the back terminals are repurposed
to power the dining room).

## The design: one real stream + a "phantom" second instance

Two `shairport-sync` instances, one per room:

- **Instance 1 — "Living Room"**: real audio output to the Digi+ (which feeds
  **both** NAD inputs via optical + coax). Its AirPlay volume maps to **NAD
  `Main.Volume`**.
- **Instance 2 — "Dining Room"**: audio output to a **null sink** (shairport's
  `pipe`/`stdout` backend to `/dev/null`, or a dummy ALSA null device). It
  **produces no audio**. It exists only to be a **second AirPlay endpoint** that
  exposes a **second volume slider**. Its AirPlay volume maps to **NAD
  `Zone2.Volume`**.

### Usage model

Cast to **both** endpoints at once using **AirPlay 2 multi-room** (group both
speakers on the phone), or cast to Living Room. Both rooms then play instance 1's
single physical stream (Main gets it over optical, Zone 2 over coax), and the
phone shows **two independent sliders** that drive the two NAD zone volumes.

**Bonus — perfect sync for free:** because both rooms are literally the same
physical S/PDIF stream, there is no multi-room clock-sync problem to solve.

### Important caveat: instance 2 is a volume proxy, not a real player

The dining room's sound **always comes from instance 1's feed**. Instance 2's own
audio is dumped. Therefore:

- Cast to **both** (multi-room), or to **Living Room** → both rooms play, both
  sliders work. ✅
- Cast to **Dining Room only** → **silence**, because instance 2 discards its
  audio and instance 1 is not feeding the Digi+. ❌

Mental model: *Living Room is the player; both endpoints are volume knobs.* This
fits the same-audio goal. If independent content per room is ever wanted, that is
exactly the point at which a second real audio output becomes necessary.

## Why D-Bus volume (not the ALSA mixer) is the enabler

With shairport `ignore_volume_control = "yes"` **and its ALSA mixer removed**,
each instance becomes a **self-contained audio-out + volume-reporter** with no
dependency on a hardware mixer control. That is what makes running **multiple
instances** (or containers) clean — otherwise each instance would need its own
ALSA mixer element, which does not multiply nicely.

Key D-Bus facts (learned the hard way this session):

- **Read the sender's live volume from `org.gnome.ShairportSync.RemoteControl`
  property `AirplayVolume`** (a dB double). It tracks the sender live, **even
  under `ignore_volume_control=yes`** (shairport still emits the `pvol` metadata
  with the sender volume as the first field).
- **Write the sender's slider via the method
  `RemoteControl.SetAirplayVolume(d)`** (a method call, dB argument) — this makes
  shairport report the level back to the sender, so the phone slider follows.
- **Do NOT use the main-interface `org.gnome.ShairportSync.Volume` property** — it
  only updates on writes/init and otherwise sits frozen. Reading it was the bug
  that made pure-D-Bus volume look impossible.
- **Safety:** the daemon enforces a **hard `max_volume` ceiling on every
  volume-set path**, so no bug or session-start transient can drive the receiver
  past it. (This matters: running shairport at full-scale with `ivc=yes` means the
  receiver's own attenuation, driven by the daemon, is the only volume control —
  the ceiling is the safety net.)

## Control plane

One `avr-controld` instance owns the **single NAD RS232 link** and runs **two
zone-mapped D-Bus watchers**. Each shairport instance runs on its **own private
D-Bus** (so both can own the `org.gnome.ShairportSync` name without colliding),
and the daemon addresses each via the existing **`AVR_DBUS_ADDRESS`** hook:

- Living-Room bus → `Main.Volume`
- Dining-Room bus → `Zone2.Volume`

NAD control is the **TXX7 ASCII protocol** over RS232 (**115200 8N1**, no flow
control), `Object.Property=Value<CR>` syntax, with objects **`Main.*`** and
**`Zone2.*`** exposing Power / Volume / Mute / Source and **pollable status
feedback**. Because the NAD **pushes zone volume changes** over the control link,
the **reverse direction** (NAD remote or front panel → phone slider via
`SetAirplayVolume`) is expected to work on the NAD — unlike the Denon, which does
not push HTTP-driven changes to its control link.

## Software work required (build checklist)

1. **NAD backend: add `Zone2.*` support.** The current NAD serial backend drives
   the main zone only; add Zone 2 Power/Volume/Mute/Source (and status parsing).
2. **Daemon: two zone-mapped D-Bus watchers.** Let `avr-controld` watch two
   shairport buses at once with a `zone → NAD object` mapping in config (one
   watcher → `Main.Volume`, the other → `Zone2.Volume`), while still owning the
   single RS232 connection and serializing access.
3. **Two shairport-sync instances.** Distinct AirPlay names ("Living Room",
   "Dining Room"), `ignore_volume_control=yes`, **mixer removed**, each on its
   **own private D-Bus**, instance 2 using a **null output**. Prefer **native
   systemd template instances** over Docker: the owner previously moved off Docker
   to native for AirPlay 2 reliability. (Docker with host networking is possible
   but adds mDNS/port fiddliness for two AirPlay 2 instances on one host.)
4. **Physical wiring.** Pi → NAD **RS232** via a USB-serial adapter; Digi+
   **optical → NAD Main input**, Digi+ **coax → NAD Zone 2 input**.
5. **NAD setup.** Assign the surround-back amp channels to **Zone 2** (living room
   then runs ≤ 5.1). NAD MDC serial/upgrade eligibility is **not** relevant here —
   RS232 control is native to the receiver and independent of the (lightning-fried)
   HDMI module.

## Open questions / decisions

- **AirPlay 2 multi-room grouping across two self-hosted shairport instances** —
  needs live testing (do two independent instances group cleanly on the phone, and
  do both sliders behave as expected?).
- **Docker vs native** for the two instances (leaning native, per above).
- **Second output path** if independent *content* per room is ever wanted later
  (USB-S/PDIF adapter or a second Pi) — deferred; not needed for same-audio.

## Relationship to existing components

- `avr-controld` already owns a single AVR connection and serves clients; the
  D-Bus watcher already accepts `AVR_DBUS_ADDRESS`. Two zone-mapped watchers is an
  extension of the existing single-watcher volume-sync design.
- The D-Bus volume path (read `AirplayVolume` / write `SetAirplayVolume`, hard
  `max_volume` cap) is the same mechanism validated this session on the Denon;
  here it is applied per-instance and mapped per-zone on the NAD.

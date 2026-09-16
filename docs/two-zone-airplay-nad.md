# Two-zone AirPlay over a NAD T 757 (Living Room + Dining Room)

Status: **Living Room is built and live; Dining Room (Zone 2) AirPlay is a plan.**
Rewritten 2026-09-16 after confirming a hard T 757 limitation that invalidates
the earlier "coax → Zone 2" design (see *What changed* at the end). Supersedes
the stale `MULTI_ZONE_AND_SHARED_CONNECTION.md` at the repo root.

## Goal

Two AirPlay endpoints on the phone — "Living Room" and "Dining Room" — both
driving one **NAD T 757**:

- **Living Room → NAD Main zone** — **done and live.**
- **Dining Room → NAD Zone 2** — **not yet possible as-wired** (see below); this
  doc is the cheapest route to make it work.

Same audio in both rooms is fine; only **volume/on-off** must be per-room.

## Hardware reality (two hard constraints)

### 1. The T 757's Zone 2 is ANALOG-ONLY (the decisive one)

Confirmed on this unit (firmware v1.38) and in NAD's docs: **the T 757 does not
route digital audio (HDMI, optical, coax) to Zone 2.** Only a source whose
**analog** audio input is connected can be used as a Zone 2 source; the T 757
cannot down-mix a digital signal to the second zone (the pricier 777/787 can).

Verified live: the **Turntable** (analog, Source 5) plays in Zone 2; **AirPlay**
(which arrives digitally via optical) plays in Main but is **silent in Zone 2**,
regardless of Zone 2 volume.

Consequence: **Zone 2 needs an analog line-level feed.** Any plan that sends the
AirPlay audio to Zone 2 over optical/coax is impossible on this receiver.

### 2. The HiFiBerry Digi+ is digital-only (optical + coax, one stream)

The Pi's Digi+ has optical **and** coax, but they are one WM8804 S/PDIF stream on
two connectors — and both are **digital**. So the existing Pi output cannot feed
Zone 2 at all (constraint 1). Getting AirPlay to Zone 2 requires **a new analog
output**.

## What is already built and live (Living Room + full NAD control)

- `avr-controld` (Python) owns the NAD over RS232 (stable `by-id` path) and
  bridges it to Home Assistant over MQTT: power, volume (0-100), mute, **source
  select** (auto-discovered names), bass, treble, **Zone 2 power/volume(%)/mute/
  source**, sleep, listening mode, dimmer, model, firmware.
- **Living-Room AirPlay → NAD Main**: shairport (optical → NAD Main), the daemon
  powers the NAD on + selects the AirPlay input when it plays, and the phone
  slider drives Main volume (D-Bus `AirplayVolume`, clamped to `max_volume`).
- **CEC volume relay**: TV/Shield remote → NAD over RS232 (audio over Toslink).
- NAD sources renamed over RS232: Source 1 = **TV** (Optical 1), Source 2 =
  **AirPlay** (Optical 2), Source 5 = **Turntable** (analog). Audio-input
  assignment itself is menu-only (not on RS232).

## The plan: cheapest AirPlay-to-Zone-2 (analog feed)

Give the Pi a cheap **analog** output and run a **second AirPlay target** on it,
wired to a NAD **analog** input that Zone 2 can use.

### Hardware (~$5-10)

- A **USB audio dongle** with a 3.5 mm line/headphone out (CM108-class, a few
  dollars; or a ~$10 USB DAC for cleaner output). Into the existing StueTærten Pi.
- A **3.5 mm → RCA** cable to a spare NAD **analog** input.

### Signal path

```
Phone ─AirPlay─▶ Pi: shairport #2 "Dining Room" ─USB DAC─▶ 3.5mm→RCA
       ─▶ NAD analog input (assigned to a source) ─▶ Zone 2 ─▶ dining-room speakers

(live today: Phone ─AirPlay─▶ Pi: shairport #1 ─optical─▶ NAD Main)
```

### Build checklist

1. **USB DAC**: plug in, find its ALSA device (`aplay -l`).
2. **Second shairport-sync instance** ("Dining Room" / "Spisestue"), AirPlay 2,
   output = the USB DAC, `ignore_volume_control=yes`, **mixer removed**. Prefer a
   native systemd template instance over Docker (the AVR Pis moved off Docker for
   AirPlay 2 reliability).
3. **NAD**: assign an unused source's **audio input = the analog input** you wired
   (on-screen menu — RS232 can't set audio inputs), name it "Dining Room". Then
   set **Zone 2 source = Dining Room** (from HA `select.stue_nad_zone_2_source`).
4. **Amp for Zone 2**: the dining-room speakers must be driven — either the T 757's
   surround-back amp channels assigned to Zone 2 in the menu (Main then runs ≤5.1),
   or an external amp on the Zone 2 pre-outs. (Menu-only; not on RS232.)

### Whole-home

Both endpoints are AirPlay 2 targets, so grouping "Living Room" + "Dining Room"
on the phone gives synced whole-home audio (Main via optical, Zone 2 via the
analog dongle), with independent per-room sliders.

## Volume / power integration for Zone 2 (make it as automatic as Main)

The daemon already powers Main and syncs its volume from shairport #1's D-Bus.
For Zone 2 the same idea applies to shairport #2, with one wrinkle:

- **D-Bus name collision**: shairport's D-Bus service name is fixed
  (`org.gnome.ShairportSync`), so two instances can't both own it on the same bus.
  Options: run instance #2 on its **own private D-Bus** and point a second daemon
  watcher at it via the existing **`AVR_DBUS_ADDRESS`** hook (map its
  `AirplayVolume` → `Zone2.Volume`), **or** consume instance #2's **MQTT** (built
  into shairport) and drive Zone 2 from the daemon / an HA automation.
- **Power demand**: shairport #2 playing → a "dining" demand → power Zone 2 on +
  select the Dining Room source (mirrors the Main power policy).
- **Safety**: apply a **Zone 2 volume ceiling** on the automated path too, exactly
  like Main's `max_volume` (Zone 2 at 0 dB is very loud).

## D-Bus volume facts (still the enabler, now per analog instance)

- Read the sender's live volume from
  `org.gnome.ShairportSync.RemoteControl.AirplayVolume` (dB double); it tracks the
  sender even under `ignore_volume_control=yes`.
- Write the phone slider via the **method** `RemoteControl.SetAirplayVolume(d)`
  (NOT the frozen main-interface `.Volume` property).
- The NAD **pushes** zone volume changes over RS232, so reverse sync (NAD
  remote/front panel → phone slider) is feasible for Zone 2 too.

## What changed from the earlier plan

The previous version fed **coax → Zone 2** and used a "phantom" null-output second
instance purely as a volume proxy, assuming Zone 2 would play the coax digital
signal. **That cannot work**: Zone 2 is analog-only (constraint 1). The second
instance must therefore produce **real analog audio** (USB DAC → NAD analog
input), not a null sink. Everything else (two instances, per-zone D-Bus volume,
AirPlay 2 grouping) carries over.

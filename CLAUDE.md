# CLAUDE.md

Guidance for working in this repository.

## What this is

**`avr-controld`**, a Python broker daemon that owns exactly one real connection to each
AV receiver (NAD T757 and Opalum over serial) and serves clients on a Unix socket and
Home Assistant over MQTT. The socket text protocol (`docs/socket-protocol.md`,
implemented in `src/avrcontrold/protocol.py`) is the contract with the C/C++ clients in
the sibling repo `martinmunk/alsa-avr-control` (ALSA ctl plugins, `avr-cec-relay`,
`avr-power`). The two repos version and release independently.

It runs on the Tærten AirPlay Pis: **StueTærten** (192.168.20.240, NAD T757 + HDMI-CEC
relay, two AirPlay zones) and **VærkstedsTærten** (192.168.20.217, Opalum).

## Working practices

- **Commit and push as you go.** Small, focused commits, pushed immediately. The remote
  is the source of truth.
- Ask which branch to use rather than assuming a branching policy.
- Run the offline suite before committing: `PYTHONPATH=src python3 -m pytest -q` (or the
  project venv `.venv`). It must stay hermetic: no hardware, no broker, no runtime deps
  (backends and MQTT import their libraries lazily).

## Build, deploy, release

- **Never build on the Pis.** Dev builds: `dch -b -v X.Y.Z~devN ...`, then
  `dpkg-buildpackage -b --no-sign -rfakeroot` in a Debian trixie container (the venv
  build needs LAN access to the Forgejo PyPI index for `opalum-receiver`), `dpkg -i` on
  the Pi, then `git checkout -- debian/changelog`. Everything on a Pi must be dpkg-tracked.
- **Releases are cut on `main` only, never from a branch**, and only via
  `./create-release.sh` (writes the changelog from git, tags `release/X.Y.Z`, pushes),
  which CI (`.forgejo/workflows/publish.yml`) builds and
  publishes to the Forgejo apt registry. Never curl-upload a package by hand. Never
  hand-edit the changelog for a release or set DEBEMAIL; identity comes from git config.
- CI runs on Forgejo (`.forgejo/`), `runs-on: docker`, Debian trixie. The package is
  Architecture: all.

## Hardware safety - read this before anything that changes volume/power

This software drives real receivers and speakers. A bug can blast speakers at full
volume. This has already happened once (shairport `ignore_volume_control=yes` plus an
experimental volume sync drove a Denon to 100% because a clamp path was missed).

- **Never enable a feature that can command receiver volume, live on a real system,
  when it rests on an untested assumption or an unproven safety cap.** Prove the clamp
  and the mapping first.
- Test any volume-affecting change with the **receiver hard-limited to a low volume
  and the audio source silent or near-silent** (e.g. 25 s of generated silence over
  AirPlay) - never from a configuration that can reach full output.
- `max_volume` in the config is the ceiling applied on every absolute volume path
  (socket, MQTT, AirPlay slider). Keep it conservative on a new deployment.
- For power/input/volume changes, take the smallest reversible step and read the
  receiver's actual state back rather than assuming the command took. The Opalum
  protocol is write-only: its state is whatever the daemon last set.

## Topology notes

- The daemon is the single owner of each serial port / receiver connection. Nothing
  else (HA integrations, the old C++ daemon, ALSA mixer plugins talking straight to the
  device) may open it.
- Home Assistant (`homeassistant.egaa.loc.martinmunk.com`) gets the receiver entities
  from this daemon's MQTT discovery and AirPlay state from shairport-sync's own MQTT.

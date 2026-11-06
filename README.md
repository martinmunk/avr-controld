# avr-controld

Broker daemon for AV receivers: one owner of the serial/telnet link (NAD T757 and
Opalum over RS232 today), serving everyone else:

- a Unix-socket text protocol (`docs/socket-protocol.md`) for the ALSA ctl plugins,
  `avr-cec-relay` and `avr-power` from the `alsa-avr-control` repo (the socket clients),
- optionally Home Assistant via MQTT discovery (power/volume/mute/input plus every
  extra the receiver exposes, and a "Receiver Link" diagnostic: the entities go
  unavailable, not stale, while the serial link is down) -- set `mqtt_host` to
  enable it, leave it unset to run as a pure socket broker,
- a power policy driven by shairport-sync over D-Bus (AirPlay playing) and by the CEC
  relay (TV active), with a hardware-safety volume ceiling on every absolute volume path.

Packaged as `martinmunk-avr-controld` (Architecture: all). See `CLAUDE.md` for working
practices, `docs/` for the socket protocol and the two-zone AirPlay setup,
`examples/config.conf` for configuration.

## Installing

The package runs on the system `python3` (3.11 or newer: Debian bookworm/trixie,
Raspberry Pi OS, Ubuntu 24.04+) and ships all Python dependencies as a private
library tree under `/opt/avr-controld/lib`, so nothing is pulled from PyPI on the
target. Either

- add the apt repository once and install/upgrade with apt:
  ```
  curl -fsSL https://martinmunk.github.io/apt/martinmunk-apt.gpg | sudo tee /usr/share/keyrings/martinmunk-apt.gpg >/dev/null
  echo "deb [signed-by=/usr/share/keyrings/martinmunk-apt.gpg] https://martinmunk.github.io/apt trixie main" | sudo tee /etc/apt/sources.list.d/martinmunk.list
  sudo apt update && sudo apt install martinmunk-avr-controld
  ```
- or download the `.deb` from the [GitHub releases](https://github.com/martinmunk/avr-controld/releases)
  and `sudo dpkg -i` it.

Then create `/etc/avr-controld/config.conf` (start from
`/usr/share/doc/martinmunk-avr-controld/examples/config.conf`) and
`sudo systemctl enable --now avr-controld-py`.

```
PYTHONPATH=src python3 -m pytest -q         # offline suite, no hardware/broker/serial needed
dpkg-buildpackage -b --no-sign -rfakeroot   # the .deb (deps fetched from public PyPI at build time)
./create-release.sh                         # tag release/X.Y.Z -> CI builds + publishes
```

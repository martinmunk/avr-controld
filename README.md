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

Packaged as `martinmunk-avr-controld` (Architecture: all, bundled venv under
`/opt/avr-controld/venv`). See `CLAUDE.md` for working practices, `docs/` for the socket
protocol and the two-zone AirPlay setup, `examples/config.conf` for configuration.

```
PYTHONPATH=src python3 -m pytest -q         # offline suite, no hardware/broker/serial needed
dpkg-buildpackage -b --no-sign -rfakeroot   # the .deb (bundles the venv from public PyPI)
./create-release.sh                         # tag release/X.Y.Z -> CI builds + publishes
```

# avr-controld

Python broker daemon that owns the one real connection to each AV receiver (NAD T757
and Opalum over RS232 today) and serves everyone else:

- a Unix-socket text protocol (`docs/socket-protocol.md`) for the ALSA ctl plugins,
  `avr-cec-relay` and `avr-power` from the `alsa-avr-control` repo (the socket clients),
- Home Assistant via MQTT discovery (power/volume/mute/input plus every extra the
  receiver exposes),
- a power policy driven by shairport-sync over D-Bus (AirPlay playing) and by the CEC
  relay (TV active), with a hardware-safety volume ceiling on every absolute volume path.

Packaged as `martinmunk-avr-controld` (Architecture: all, bundled venv under
`/opt/avr-controld/venv`). See `CLAUDE.md` for working practices, `docs/` for the socket
protocol and the two-zone AirPlay setup, `examples/config.conf` for configuration.

```
PYTHONPATH=src python3 -m pytest -q         # offline suite, no hardware/broker/serial needed
dpkg-buildpackage -b --no-sign -rfakeroot   # the .deb (needs LAN access to the Forgejo PyPI index)
./create-release.sh                         # tag release/X.Y.Z -> CI builds + publishes
```

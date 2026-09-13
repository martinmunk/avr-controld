# ALSA AVR Control Plugin

A modular ALSA control plugin for controlling AV receivers through various backends. Integrates seamlessly with `alsamixer`, allowing your AVR volume to be controlled as a standard ALSA mixer control.

**NEW:** Separate plugins per backend are now available! Each backend can be built as a standalone plugin, resulting in smaller binaries and simpler configuration.

## Supported Backends

### 1. Denon IP (Ethernet)
- **Protocol:** Telnet (port 23)
- **Features:** Absolute volume, query volume, mute control, power control
- **Tested with:** Denon AVR-X1100W
- **Connection:** Ethernet/WiFi

### 2. NAD UART (Serial/Ethernet)
- **Protocol:** NAD v2.X ASCII protocol (115200 baud, 8N1 or TCP port 23)
- **Features:** Absolute volume in dB (-90 to 0), query volume, mute control, power control
- **Tested with:** NAD T-series receivers
- **Connection:** RS-232 or Ethernet (same protocol for both)

### 3. Opalum RS232 (Serial)
- **Protocol:** RS232 (115200 baud, 8N1)
- **Features:** Absolute volume (0-99), power control, source selection
- **Hardware:** Uses 3.5mm jack for serial connection
- **Connection:** Serial port (USB-to-serial adapter supported)

### 4. CEC (HDMI)
- **Protocol:** HDMI-CEC via `cec-client`
- **Features:** Incremental volume only (no query support)
- **Limitations:** Cannot read current volume, toggle-only mute
- **Connection:** HDMI (requires CEC-capable hardware)

## Building

### Prerequisites
```bash
# Debian/Ubuntu
sudo apt-get install libasound2-dev cmake g++ libcec-dev

# Arch Linux
sudo pacman -S alsa-lib cmake gcc libcec
```

### Compile
```bash
cd cpp
mkdir build && cd build
cmake ..
make
```

### Install
```bash
sudo make install
```

This installs:
- Plugin libraries to your system's ALSA plugin directory (typically `/usr/lib/alsa-lib/` or `/usr/lib/x86_64-linux-gnu/alsa-lib/`)
- Configuration files to `/etc/alsa/conf.d/` for system-wide device definitions

The build process creates:
- **libasound_module_ctl_denon.so** (116K) - Denon-only plugin
- **libasound_module_ctl_nad.so** (259K) - NAD-only plugin
- **libasound_module_ctl_opalum.so** (116K) - Opalum-only plugin
- **libasound_module_ctl_cec.so** (68K) - CEC-only plugin

## Configuration

### Plugin Types

This project provides separate ALSA plugins for each backend:

- **denon**: Denon/Marantz AVR via TCP/IP (telnet protocol)
- **nad**: NAD amplifiers via serial/RS-232
- **opalum**: Opalum amplifiers via serial/RS-232
- **cec**: HDMI-CEC control via cec-client

Each backend has its own plugin with a direct type name, smaller binary size,
and simpler configuration (no `backend` parameter needed).

### Configuration Methods

There are two ways to configure the AVR control plugins:

**Option 1: System-wide definitions (Recommended)**
Use `/etc/alsa/conf.d/` for predefined device names that work system-wide. The `sudo make install` command automatically installs configuration files that define:
- `denon_avr` - Denon/Marantz control
- `nad_serial` - NAD amplifier control
- `opalum_serial` - Opalum amplifier control
- `cec_audio` - HDMI-CEC control

After installation, you can use these directly:
```bash
amixer -D denon_avr set 'Master' 50%
alsamixer -D nad_serial
```

Then configure your specific hardware in `~/.asoundrc`:
```
ctl.!default {
    type denon
    host "192.168.1.100"
    input "MPLAY"
}
```

**Option 2: Direct configuration**
Create your own control device definitions in `~/.asoundrc` or `/etc/asound.conf`.

See `examples/conf.d/README.md` for detailed information about system-wide configuration.

### 1. ALSA Configuration File

Create or edit `~/.asoundrc` (user-specific) or `/etc/asound.conf` (system-wide).

**Denon IP Example:**
```
ctl.avr {
    type denon
    host "192.168.1.100"
    port "23"           # Optional, defaults to 23
    input "MPLAY"       # Optional: the input avr-power selects for a session
}

ctl.!default {
    type denon
    host "192.168.1.100"
}
```

**NAD Serial Example:**
```
ctl.avr {
    type nad
    device "/dev/ttyUSB0"
}

ctl.!default {
    type nad
    device "/dev/ttyUSB0"
}
```

**Opalum Serial Example:**
```
ctl.avr {
    type opalum
    device "/dev/ttyUSB0"
    input "1"           # Optional: the input avr-power selects for a session
}

ctl.!default {
    type opalum
    device "/dev/ttyUSB0"
}
```

**CEC Example:**
```
ctl.avr {
    type cec
    initial_volume "50"  # Optional
}

ctl.!default {
    type cec
}
```

**About `input`:** it is published by the plugin as a read-only `Input Source
Default` mixer element and applied by `avr-power on` (see below), not by the
plugin itself. The connection is owned by the long-running `avr-controld`, so a
switch "on open" would happen once per daemon lifetime, not once per session.
`avr-power on` saves the current power/input state, selects the configured
input *before* powering on (Denon receivers then power on straight into it,
which avoids the HDMI-CEC wake-up cascade), and `avr-power restore` restores
the input only if the receiver stays on, then powers it off if it was off.


See `examples/` directory for complete configuration files.

### 2. Serial Port Permissions (for Opalum)

Add your user to the `dialout` group:
```bash
sudo usermod -a -G dialout $USER
```

Then log out and log back in.

### 3. CEC Setup (for CEC backend)

Install cec-client:
```bash
sudo apt-get install cec-utils
```

Test CEC connectivity:
```bash
echo "scan" | cec-client -s
```

## Usage

### With alsamixer
```bash
alsamixer -c avr
```

You'll see "Master Playback Volume", "Master Playback Switch", and "Master Playback Power" controls.

### With amixer
```bash
# Set volume to 50%
amixer -c avr set 'AVR Volume' 50%

# Volume up
amixer -c avr set 'AVR Volume' 1+

# Mute
amixer -c avr set 'AVR Mute' on

# Unmute
amixer -c avr set 'AVR Mute' off
```

### Testing Backends

Use the test program to verify backend functionality:

```bash
# Test Denon IP
./test_backend denon_ip host=192.168.1.100

# Test NAD UART
./test_backend nad_uart device=/dev/ttyUSB0

# Test Opalum Serial
./test_backend opalum_serial device=/dev/ttyUSB0

# Test CEC
./test_backend cec
```

Interactive commands:
- `v <0-100>` - Set volume
- `+` - Volume up
- `-` - Volume down
- `m` - Toggle mute
- `q` - Query current state
- `x` - Exit

## shairport-sync D-Bus integration (optional)

`avr-controld` can watch shairport-sync over D-Bus and power the AVR on/off from
real playback, instead of the coarse `run_this_*_active_state` shell hooks. It
powers on and selects the configured input when playback starts (verifying via
its own live reads and retrying), and powers off after a grace period once
playback stops, so brief pauses do not cycle the receiver. It also caches
now-playing metadata for a future display or MQTT bridge.

This is **off by default** and additive: volume and mute stay on the ALSA
hardware mixer. Enable it per node by creating `/etc/avr-controld.conf`:

```
# off (default) | observe (log only, never touch the AVR) | drive
dbus=drive
# seconds after playback stops before powering the AVR off (default 120)
dbus_off_grace=180
```

With `dbus=drive`, remove the `run_this_before/after_active_state` avr-power
lines from `shairport-sync.conf` so both do not drive power. Start with
`dbus=observe` to confirm the daemon's log shows your play/pause and now-playing
before letting it control the receiver. Requires shairport-sync built with
D-Bus (`-dbus` in `shairport-sync -V`). See `DAEMON_DESIGN.md` for details and
the `META` / `PLAYSTATE` socket verbs.

### Bidirectional volume sync (optional)

`avr-controld` can also keep the AirPlay volume and the receiver's volume/mute in
step in both directions -- so changing volume from the AVR remote, HDMI-CEC or
Home Assistant moves the phone's AirPlay slider, and vice versa -- while the
audio stays full-range digital (the receiver does the attenuation, best quality).

**This is off by default.** To enable it, shairport-sync must (a) not attenuate
and (b) NOT drive a hardware mixer, so the daemon is the ONLY thing that sets the
receiver's volume:

```
# in shairport-sync.conf:
#   general { }:
ignore_volume_control = "yes";
#   alsa { }: REMOVE the mixer lines, or shairport drives the receiver to MAX
#   under ignore_volume_control (this caused a full-volume incident). Keep only
#   output_device; delete/comment:
#     mixer_control_name = ...
#     mixer_device       = ...
```
```
# in /etc/avr-controld.conf:
volume_sync=on
max_volume=40          # HARD ceiling (0-100); keep it low. Enforced on EVERY path.
volume_coalesce_ms=150 # smooth fast slider drags
```

`max_volume` is a hard ceiling enforced on every volume-set path in the daemon:
a phone at full slider, a wrong value, or a stray `SET volume` can never drive
the AVR past it. Two rules, both mandatory:

- Do NOT set `ignore_volume_control=yes` WITHOUT `volume_sync=on` -- the AVR would
  play full-scale with nothing controlling it.
- Do NOT leave shairport's `mixer_device` set WITH `volume_sync=on` -- shairport
  drives the receiver to maximum, fighting (and beating) the daemon's cap.

`avr-power` keeps its own handle for power/input, so removing the volume mixer
does not affect power/input control. Requires shairport-sync built with D-Bus.
See `DAEMON_DESIGN.md`.

## Architecture

```
ALSA Application (alsamixer, amixer)
           ↓
    ALSA Control API
           ↓
    AVR Control Plugin
           ↓
    Backend Interface
      ↙    ↓    ↘
  Denon  Opalum  CEC
   IP     RS232
```

### Backend Abstraction

All backends implement the same interface:
- `set_volume_absolute(level)` - Set specific volume (0-100)
- `volume_up()` / `volume_down()` - Incremental control
- `get_volume()` - Query current volume
- `set_mute(bool)` / `get_mute()` - Mute control
- `set_power(bool)` / `get_power()` - Power control

### Capabilities

Each backend reports its capabilities:
- **supports_absolute**: Can set specific volume levels
- **supports_query**: Can read current volume from device
- **supports_mute**: Has native mute control

The plugin automatically adapts based on these capabilities.

## Troubleshooting

### Plugin not loading
Check ALSA plugin directory:
```bash
ls -la /usr/lib/alsa-lib/libasound_module_ctl_avr.so
# or
ls -la /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_ctl_avr.so
```

Test plugin loading:
```bash
aplay -L | grep avr
```

### Denon IP: Connection timeout
- Verify receiver IP address: `ping 192.168.1.100`
- Check receiver is on and network is enabled
- Ensure port 23 (telnet) is not blocked by firewall

### Opalum: Permission denied on /dev/ttyUSB0
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in
```

### CEC: cec-client not found
```bash
sudo apt-get install cec-utils
```

### Volume not syncing
For backends without query support (CEC), the plugin tracks volume internally. If the receiver volume was changed externally (e.g., via remote control), the plugin won't know about it.

Solution: Use backends with query support (Denon IP, future RS232 protocols with query) or restart the application.

## Adding New Backends

1. Create a new backend class in `src/backends/your_backend.cpp`
2. Implement the `Backend` interface
3. Create factory function `create_your_backend()`
4. Register in `src/backend_factory.cpp`
5. Update CMakeLists.txt

Example backend skeleton:
```cpp
class MyBackend : public Backend {
public:
    bool init(const std::map<std::string, std::string>& config) override {
        // Initialize connection
        return true;
    }

    BackendCaps capabilities() const override {
        return {
            .supports_absolute = true,
            .supports_query = false,
            .supports_mute = true,
            .min_volume = 0,
            .max_volume = 100
        };
    }

    // Implement other methods...
};

std::unique_ptr<Backend> create_my_backend() {
    return std::make_unique<MyBackend>();
}
```

## Protocol Documentation

### Denon IP Protocol
- Commands: Plain ASCII terminated with `\r`
- Volume: `MV00`-`MV99` (set), `MVUP`/`MVDOWN` (increment), `MV?` (query)
- Mute: `MUON`/`MUOFF`, `MU?` (query)
- Power: `PWON`/`PWSTANDBY`, `PW?` (query)

### NAD UART Protocol
- Baud: 115200, 8N1, no flow control (or TCP port 23 for Ethernet)
- Format: `Prefix.Variable<Op><Value>\r`
- Volume: `Main.Volume=-30` (dB), `Main.Volume+`/`Main.Volume-`, `Main.Volume?`
- Mute: `Main.Mute=On`/`Main.Mute=Off`, `Main.Mute?`
- Power: `Main.Power=On`/`Main.Power=Standby`, `Main.Power?`
- Response: Echoes command with actual value (e.g., `Main.Volume=-30\r`)

### Opalum RS232 Protocol
- Baud: 115200, 8N1, no flow control
- Commands: ASCII terminated with `\r`
- Volume: `vol[00-63]` (hex), `vol+`/`vol-`
- Power: `pwr1`/`pwr0`
- Response: `OK` or `ER`

### CEC Protocol
Uses `cec-client` command-line tool:
- Volume: `volup` / `voldown`
- Mute: `mute` (toggle)
- Power: `on 5` / `standby 5` (audio system address = 5)

## Docker Usage

The AVR control plugin can be used in Docker containers. Key points:

### What You Need

**For network-based backends (Denon IP, NAD TCP):**
- `network_mode: host` OR port forwarding
- Plugin library and ALSA config in container
- **NO /dev/snd mapping needed** (control only)

**For serial backends (NAD UART, Opalum RS232):**
- Map serial device: `--device=/dev/ttyUSB0:/dev/ttyUSB0`
- Plugin library and ALSA config in container
- **NO /dev/snd mapping needed** (control only)

**For audio playback + AVR control:**
- Map audio hardware: `--device=/dev/snd:/dev/snd`
- Network access OR serial device for AVR control
- Plugin library and ALSA config

### Example: Denon IP Control

```bash
docker run --rm --network host \
  -v /path/to/libasound_module_ctl_avr.so:/usr/lib/alsa-lib/libasound_module_ctl_avr.so:ro \
  -v /path/to/asound.conf:/etc/asound.conf:ro \
  debian:bookworm-slim \
  bash -c "apt-get update && apt-get install -y alsa-utils && amixer -D avr set 'AVR' 50%"
```

### Example: NAD UART Control

```bash
docker run --rm \
  --device=/dev/ttyUSB0:/dev/ttyUSB0 \
  -v /path/to/libasound_module_ctl_avr.so:/usr/lib/alsa-lib/libasound_module_ctl_avr.so:ro \
  -v /path/to/asound.conf:/etc/asound.conf:ro \
  debian:bookworm-slim \
  bash -c "apt-get update && apt-get install -y alsa-utils && amixer -D avr set 'AVR' 60%"
```

### Using Docker Compose

See `docker-compose.yml` for complete examples including audio playback scenarios.

### Important Notes

- **/dev/snd is NOT required** for AVR control - only for audio playback
- The plugin communicates directly with AVR (network/serial), not through sound cards
- For PulseAudio/PipeWire integration, map appropriate sockets
- Container needs libasound2 package installed

## License

MIT License - see LICENSE file for details.

## Contributing

Contributions welcome! Please submit pull requests or issues on GitHub.

Areas for improvement:
- Additional backend support (Yamaha, Onkyo, etc.)
- Better error handling and logging
- Volume curve mapping
- Multi-zone support
- D-Bus integration

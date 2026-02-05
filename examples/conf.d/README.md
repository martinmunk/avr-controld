# ALSA Configuration Directory Files

This directory contains example ALSA configuration files that can be installed to `/etc/alsa/conf.d/` for system-wide availability of AVR control devices.

## Installation

Copy the relevant configuration file(s) to `/etc/alsa/conf.d/`:

```bash
# For Denon/Marantz AVR
sudo cp 50-denon-avr.conf /etc/alsa/conf.d/

# For NAD amplifiers
sudo cp 50-nad-serial.conf /etc/alsa/conf.d/

# For Opalum amplifiers
sudo cp 50-opalum-serial.conf /etc/alsa/conf.d/

# For HDMI-CEC control
sudo cp 50-cec-control.conf /etc/alsa/conf.d/
```

## Usage

### Method 1: Use Predefined Device Names

After installing the configuration files, you can use the predefined device names directly:

```bash
# Using default parameters
amixer -D denon_avr set 'Master' 50%
amixer -D nad_serial set 'Master' 60%
amixer -D opalum_serial set 'Master' 70%
amixer -D cec_audio set 'Master' 80%

# Using alsamixer
alsamixer -D denon_avr
```

### Method 2: Set as Default Device

Create `~/.asoundrc` or `/etc/asound.conf` to make one of these your default control device:

```
# Denon AVR as default with custom parameters
ctl.!default {
    type denon
    host "192.168.1.100"
    port "23"
    input "MPLAY"
    log_file "/var/log/alsa-avr/denon.log"
    log_level "INFO"
}
```

```
# NAD Serial as default
ctl.!default {
    type nad
    device "/dev/ttyUSB0"
    log_file "/var/log/alsa-avr/nad.log"
    log_level "INFO"
}
```

```
# Opalum Serial as default with auto input switching
ctl.!default {
    type opalum
    device "/dev/ttyUSB0"
    input "1"
    log_file "/var/log/alsa-avr/opalum.log"
    log_level "DEBUG"
}
```

```
# CEC as default
ctl.!default {
    type cec
    initial_volume "50"
    log_file "/var/log/alsa-avr/cec.log"
}
```

### Method 3: Include in Your Own Configuration

You can also reference the predefined devices in your own ALSA configuration:

```
# Use denon_avr with custom parameters
ctl.myavr {
    type denon
    host "192.168.1.50"
    input "BD"
}

# Set as default
ctl.!default {
    type denon
    host "192.168.1.50"
}
```

## File Naming Convention

The `50-*` prefix follows ALSA's standard naming convention:
- `10-*`: Early loading (rate converters, etc.)
- `50-*`: Plugin definitions
- `60-*`: Audio processing
- `99-*`: Default configurations

Files are loaded in alphabetical order.

## Benefits of /etc/alsa/conf.d/

1. **System-wide availability**: All users can access the control devices
2. **No user configuration needed**: Works out of the box with default parameters
3. **Cleaner separation**: Plugin definitions separate from user preferences
4. **Package integration**: Can be installed by package managers (deb/rpm)
5. **Discovery**: Devices appear in ALSA device listings

## Verifying Installation

Check if the device is available:

```bash
# List all ALSA control devices
aplay -L | grep -A2 denon_avr

# Test the device
amixer -D denon_avr info
```

## Logging

Create log directory if using logging:

```bash
sudo mkdir -p /var/log/alsa-avr
sudo chmod 777 /var/log/alsa-avr  # Or set appropriate permissions
```

## Notes

- Configuration files are loaded every time an ALSA device is opened
- Changes take effect immediately without restarting
- User configurations in `~/.asoundrc` override system-wide settings
- The `/etc/alsa/conf.d/` directory is managed by ALSA and loaded automatically

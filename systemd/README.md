# Systemd Service Configuration

This directory contains a generic systemd service file and per-Pi configuration files for running Shairport Sync with AVR control plugins.

## Files

- `shairport-sync-avr.service` - Generic systemd service file (same for all Pis)
- `asound.conf` - Generic ALSA configuration (uses environment variables, same for all Pis)
- `service.conf.pi116-opalum` - Configuration for Pi 116 (VærkstedsTærten/Opalum)
- `service.conf.pi240-denon` - Configuration for Pi 240 (StueTærten/Denon)

## Installation

### 1. Copy the systemd service file

```bash
sudo cp shairport-sync-avr.service /etc/systemd/system/shairport-sync-docker.service
```

### 2. Create the configuration directory

```bash
sudo mkdir -p /etc/shairport-sync-avr
```

### 3. Copy the appropriate configuration for your Pi

**For Pi 116 (Opalum):**
```bash
sudo cp service.conf.pi116-opalum /etc/shairport-sync-avr/service.conf
```

**For Pi 240 (Denon):**
```bash
sudo cp service.conf.pi240-denon /etc/shairport-sync-avr/service.conf
```

### 4. Copy generic ALSA configuration

The generic asound.conf file uses environment variables from service.conf to configure all backends:

```bash
# For Docker container
sudo cp asound.conf /etc/shairport-sync-avr/asound.conf

# For host system
sudo cp asound.conf /etc/asound.conf
```

This single file works for all backends (denon, opalum, nad, cec) by reading environment variables like `DENON_HOST`, `OPALUM_DEVICE`, etc.

**Note**: The Docker container reads environment variables from service.conf. On the host, you can either:
- Set environment variables before running ALSA commands: `DENON_HOST=192.168.20.107 amixer -D denon`
- Use the default values defined in asound.conf

### 5. Reload systemd and enable the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable shairport-sync-docker.service
sudo systemctl start shairport-sync-docker.service
```

## Configuration

All configuration is done by editing `/etc/shairport-sync-avr/service.conf`. The file contains environment variables that configure both the systemd service and the ALSA backends:

### SERIAL_DEVICE_MOUNT
Mount a serial device into the Docker container for serial-based AVR backends.

**Example (for serial backends like Opalum, NAD):**
```bash
SERIAL_DEVICE_MOUNT=--device=/dev/ttyUSB1:/dev/ttyUSB1
```

**Example (for IP backends like Denon, leave empty):**
```bash
SERIAL_DEVICE_MOUNT=
```

### PLUGIN_MOUNTS
Mount the ALSA plugins from the host into the container.

**Recommended (mount entire directory):**
```bash
PLUGIN_MOUNTS=-v /usr/local/lib/alsa-lib-docker:/usr/lib/alsa-lib:ro
```

This mounts all Alpine/musl-compiled plugins from `/usr/local/lib/alsa-lib-docker/` (on host) into `/usr/lib/alsa-lib/` (in container).

**Note**: The host uses glibc-compiled plugins in `/usr/lib/alsa-lib/` and `/usr/lib/aarch64-linux-gnu/alsa-lib/`, while Docker containers use musl-compiled plugins from `/usr/local/lib/alsa-lib-docker/`. This separation is necessary because glibc and musl are binary incompatible.

### ALSA Backend Configuration

Environment variables that configure the generic asound.conf file. Set the variables for your backend:

**For Denon IP backend:**
```bash
DENON_HOST=192.168.20.107
DENON_PORT=23
DENON_INPUT=MPLAY
DENON_LOG_FILE=/var/log/alsa-avr/denon.log
DENON_LOG_LEVEL=INFO
```

**For Opalum Serial backend:**
```bash
OPALUM_DEVICE=/dev/ttyUSB1
OPALUM_INPUT=5
OPALUM_LOG_FILE=/var/log/alsa-avr/opalum.log
OPALUM_LOG_LEVEL=DEBUG
```

**For NAD Serial backend:**
```bash
NAD_DEVICE=/dev/ttyUSB0
NAD_LOG_FILE=/var/log/alsa-avr/nad.log
NAD_LOG_LEVEL=INFO
```

**For CEC backend:**
```bash
CEC_INITIAL_VOLUME=50
CEC_LOG_FILE=/var/log/alsa-avr/cec.log
CEC_LOG_LEVEL=INFO
```

### SHAIRPORT_FLAGS
Command-line flags passed to shairport-sync.

**Required flags:**
- `-o alsa` - Use ALSA output
- `-d <device>` - ALSA device name (matches your asound.conf)
- `-c Master` - Use the "Master" mixer control

**Optional flags:**
- `-vvv` - Verbose logging (triple verbose)
- `--statistics` - Show statistics
- `-a "My AirPlay Name"` - Set custom AirPlay name

**Example:**
```bash
SHAIRPORT_FLAGS=-o alsa -d opalum -c Master -vvv --statistics -a "Workshop Speakers"
```

## Customization Examples

### Change Serial Device

Edit `/etc/shairport-sync-avr/service.conf`:
```bash
SERIAL_DEVICE_MOUNT=--device=/dev/ttyUSB0:/dev/ttyUSB0
```

Then restart:
```bash
sudo systemctl restart shairport-sync-docker.service
```

### Change ALSA Device or Mixer

Edit `/etc/shairport-sync-avr/service.conf`:
```bash
SHAIRPORT_FLAGS=-o alsa -d my_device -c Master -vvv
```

Then restart:
```bash
sudo systemctl restart shairport-sync-docker.service
```

### Use Different Plugin Directory

If you need to use a different plugin directory, edit `/etc/shairport-sync-avr/service.conf`:
```bash
PLUGIN_MOUNTS=-v /path/to/your/plugins:/usr/lib/alsa-lib:ro
```

The default configuration mounts all plugins from `/usr/local/lib/alsa-lib-docker/`.

### Change AirPlay Name

Edit `/etc/shairport-sync-avr/service.conf`:
```bash
SHAIRPORT_FLAGS=-o alsa -d denon -c Master -a "Living Room Speakers"
```

## Testing on Host

You can test the ALSA plugins directly on the host using amixer. The generic asound.conf is installed to `/etc/asound.conf`.

**Test with environment variables (Pi 240 example):**
```bash
DENON_HOST=192.168.20.107 DENON_PORT=23 amixer -D denon scontrols
DENON_HOST=192.168.20.107 amixer -D denon cget name='Master'
DENON_HOST=192.168.20.107 amixer -D denon cset name='Master' 50
```

**Test with environment variables (Pi 116 example):**
```bash
OPALUM_DEVICE=/dev/ttyUSB1 amixer -D opalum scontrols
OPALUM_DEVICE=/dev/ttyUSB1 amixer -D opalum cget name='Master'
```

**Or use the defaults from asound.conf:**
```bash
amixer -D denon scontrols   # Uses default DENON_HOST=192.168.1.100
amixer -D opalum scontrols  # Uses default OPALUM_DEVICE=/dev/ttyUSB0
```

## Viewing Logs

```bash
# View service status
sudo systemctl status shairport-sync-docker.service

# View live logs
sudo journalctl -u shairport-sync-docker.service -f

# View Docker container logs
docker logs -f shairport-sync-docker.service
```

## Version Control

The generic `shairport-sync-avr.service` file and example `service.conf.*` files are tracked in git. The actual `/etc/shairport-sync-avr/service.conf` on each Pi is NOT tracked in git, allowing each Pi to have its own configuration.

## Troubleshooting

### Service won't start
Check the configuration file syntax:
```bash
cat /etc/shairport-sync-avr/service.conf
```

Check systemd logs:
```bash
sudo journalctl -u shairport-sync-docker.service -n 50
```

### Wrong plugin loaded
Verify the plugin mount:
```bash
docker exec shairport-sync-docker.service ls -la /usr/lib/alsa-lib/
```

### Serial device not accessible
Check device permissions:
```bash
ls -la /dev/ttyUSB*
```

Make sure the device is mounted:
```bash
docker inspect shairport-sync-docker.service | grep -A 5 Devices
```

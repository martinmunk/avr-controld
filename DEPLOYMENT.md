# Deployment Guide

## Docker + Shairport-Sync Deployment

### Configuration Files

The ALSA configuration file (`asound.conf`) is externally mounted into the container, allowing changes without rebuilding:

**Location on host:** `/etc/shairport-sync-avr/asound.conf`

### Initial Setup

1. **Create config and log directories:**
   ```bash
   sudo mkdir -p /etc/shairport-sync-avr
   sudo mkdir -p /var/log/alsa-avr
   sudo cp docker-asound.conf /etc/shairport-sync-avr/asound.conf
   ```

2. **Install systemd service:**
   ```bash
   sudo cp shairport-sync-docker.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable shairport-sync-docker.service
   ```

3. **Build and start:**
   ```bash
   docker build -f Dockerfile.shairport -t shairport-sync-avr:latest .
   sudo systemctl start shairport-sync-docker.service
   ```

### Modifying Configuration

To change AVR settings (host, port, input):

1. **Edit the config file:**
   ```bash
   sudo nano /etc/shairport-sync-avr/asound.conf
   ```

2. **Restart the service:**
   ```bash
   sudo systemctl restart shairport-sync-docker.service
   ```

**No container rebuild required!**

### Example asound.conf

**Denon IP Backend:**
```
ctl.avr {
    type avr
    backend "denon_ip"
    host "192.168.20.107"
    port "23"
    input "MPLAY"
    log_file "/var/log/alsa-avr/denon.log"
    log_level "INFO"
}
```

**NAD Serial Backend:**
```
ctl.avr {
    type avr
    backend "nad_serial"
    device "/dev/ttyUSB0"
    log_file "/var/log/alsa-avr/nad.log"
    log_level "INFO"
}
```

### Available Backend Parameters

#### denon_ip
- `host` - IP address of Denon AVR (required)
- `port` - Telnet port (default: 23)
- `input` - Input to switch to on playback (e.g., "MPLAY", "BD", "DVD")
- `log_file` - Path to log file (optional, e.g., "/var/log/alsa-avr/denon.log")
- `log_level` - Logging level: DEBUG, INFO, WARN, ERROR (default: INFO)

#### nad_serial
- `device` - Serial device path (required, e.g., "/dev/ttyUSB0")
- `log_file` - Path to log file (optional, e.g., "/var/log/alsa-avr/nad.log")
- `log_level` - Logging level: DEBUG, INFO, WARN, ERROR (default: INFO)
- **Note:** Requires `--device /dev/ttyUSB0` in Docker run command

### Service Management

```bash
# Check status
sudo systemctl status shairport-sync-docker.service

# View logs
sudo journalctl -u shairport-sync-docker.service -f

# Restart service
sudo systemctl restart shairport-sync-docker.service

# Stop service
sudo systemctl stop shairport-sync-docker.service
```

### Viewing Logs

**AVR plugin logs:**
```bash
# View log file (adjust filename based on your backend)
tail -f /var/log/alsa-avr/nad.log
# or for Denon:
tail -f /var/log/alsa-avr/denon.log

# View recent logs
tail -n 100 /var/log/alsa-avr/nad.log
```

**Docker/shairport-sync logs:**
```bash
sudo journalctl -u shairport-sync-docker.service -f
```

**Note:** Errors from the AVR plugin are written to both the log file and stderr (visible in journalctl).

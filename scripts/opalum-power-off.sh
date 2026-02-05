#!/bin/sh
# Opalum Amplifier Power Off Script
# Sends pwr0 command via serial

DEVICE="${OPALUM_DEVICE:-/dev/ttyUSB0}"

log() {
    echo "[opalum-power] $1"
    logger -t "opalum-power" "$1" 2>/dev/null || true
}

log "Powering OFF Opalum at $DEVICE"

# Configure serial port (115200 8N1) and send command
if stty -F "$DEVICE" 115200 cs8 -cstopb -parenb 2>/dev/null; then
    if printf 'pwr0\r' > "$DEVICE" 2>/dev/null; then
        sleep 0.5
        log "Power OFF command sent successfully"
        exit 0
    fi
fi

log "Failed to send power OFF command"
exit 1

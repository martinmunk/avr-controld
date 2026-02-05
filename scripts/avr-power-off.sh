#!/bin/bash
# AVR Power Off (Standby) Script for shairport-sync
# Uses ALSA control interface - works with any backend (Denon, Opalum, NAD, CEC)

# Get the ALSA device from environment or use default
ALSA_DEVICE="${AVR_ALSA_DEVICE:-default}"

# Log to syslog if available
log() {
    if command -v logger &> /dev/null; then
        logger -t "avr-power" "$1"
    fi
    echo "$1"
}

log "Powering OFF receiver (device: $ALSA_DEVICE)"

# Set power control to OFF (0)
if amixer -D "$ALSA_DEVICE" cset name='Master Playback Power' 0 > /dev/null 2>&1; then
    log "Receiver powered OFF successfully"
    exit 0
else
    log "Failed to power OFF receiver"
    exit 1
fi

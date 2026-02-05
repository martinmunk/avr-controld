#!/bin/sh
# Denon AVR Power On Script
# Sends PWON command via telnet

HOST="${DENON_HOST:-192.168.20.216}"
PORT="${DENON_PORT:-23}"

log() {
    echo "[denon-power] $1"
    logger -t "denon-power" "$1" 2>/dev/null || true
}

log "Powering ON Denon at $HOST:$PORT"

# Send power on command using printf and nc
# The \r is required for Denon protocol
if printf 'PWON\r' | nc -w 3 "$HOST" "$PORT" > /dev/null 2>&1; then
    log "Power ON command sent successfully"
    exit 0
else
    log "Failed to send power ON command"
    exit 1
fi

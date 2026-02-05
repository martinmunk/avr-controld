#!/bin/sh
# Denon AVR Power Off (Standby) Script
# Sends PWSTANDBY command via telnet

HOST="${DENON_HOST:-192.168.20.216}"
PORT="${DENON_PORT:-23}"

log() {
    echo "[denon-power] $1"
    logger -t "denon-power" "$1" 2>/dev/null || true
}

log "Powering OFF Denon at $HOST:$PORT"

# Send standby command using printf and nc
if printf 'PWSTANDBY\r' | nc -w 3 "$HOST" "$PORT" > /dev/null 2>&1; then
    log "Power OFF command sent successfully"
    exit 0
else
    log "Failed to send power OFF command"
    exit 1
fi

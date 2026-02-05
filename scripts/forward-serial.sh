#!/bin/bash
# forward-serial.sh - Forward remote serial ports to local PTYs via ser2net
#
# Usage:
#   ./forward-serial.sh start   - Start all serial port forwarding
#   ./forward-serial.sh stop    - Stop all serial port forwarding
#   ./forward-serial.sh status  - Show status of forwarded ports
#
# This creates local PTY devices that connect to remote serial ports:
#   /tmp/ttyOpalum -> Pi 116 (192.168.20.116:4000) -> /dev/ttyUSB0

set -e

# Configuration
OPALUM_HOST="192.168.20.116"
OPALUM_PORT="4000"
OPALUM_PTY="/tmp/ttyOpalum"

PIDFILE_DIR="/tmp/serial-forward"

start_forward() {
    local name="$1"
    local host="$2"
    local port="$3"
    local pty="$4"

    mkdir -p "$PIDFILE_DIR"
    local pidfile="$PIDFILE_DIR/${name}.pid"

    # Check if already running
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name: Already running (PID $(cat "$pidfile"))"
        return 0
    fi

    # Check if remote port is reachable
    if ! nc -z -w2 "$host" "$port" 2>/dev/null; then
        echo "$name: Remote $host:$port is not reachable"
        return 1
    fi

    # Start socat in background with auto-restart wrapper
    # - raw,echo=0: raw mode, no echo
    # - b115200: set baud rate (informational for PTY)
    # The wrapper loop restarts socat if the connection drops
    (
        while true; do
            socat -d \
                pty,link="$pty",raw,echo=0 \
                tcp:"$host":"$port" \
                2>>"$PIDFILE_DIR/${name}.log"

            # Check if we should exit (pidfile removed = stop requested)
            [ ! -f "$pidfile" ] && break

            echo "$(date '+%Y/%m/%d %H:%M:%S') Reconnecting..." >> "$PIDFILE_DIR/${name}.log"
            sleep 1
        done
    ) &

    local pid=$!
    echo "$pid" > "$pidfile"

    # Wait for PTY to be created
    for i in 1 2 3 4 5; do
        sleep 0.5
        [ -e "$pty" ] && break
    done

    if [ -e "$pty" ]; then
        # Make PTY accessible
        chmod 666 "$pty" 2>/dev/null || true
        echo "$name: Started (PID $pid) -> $pty"
    else
        echo "$name: Started (PID $pid), PTY creation pending..."
    fi
}

stop_forward() {
    local name="$1"
    local pidfile="$PIDFILE_DIR/${name}.pid"

    if [ -f "$pidfile" ]; then
        local pid=$(cat "$pidfile")
        rm -f "$pidfile"  # Remove first to signal wrapper to exit

        if kill -0 "$pid" 2>/dev/null; then
            # Kill the wrapper and all child processes
            pkill -P "$pid" 2>/dev/null || true
            kill "$pid" 2>/dev/null || true
            echo "$name: Stopped (PID $pid)"
        else
            echo "$name: Not running"
        fi
    else
        echo "$name: Not running"
    fi

    # Also kill any orphaned socat processes for this connection
    pkill -f "socat.*$PIDFILE_DIR/${name}" 2>/dev/null || true
}

status_forward() {
    local name="$1"
    local pty="$2"
    local pidfile="$PIDFILE_DIR/${name}.pid"

    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        local pid=$(cat "$pidfile")
        if [ -e "$pty" ]; then
            echo "$name: Running (PID $pid) -> $pty"
        else
            echo "$name: Running (PID $pid), PTY not yet created"
        fi
    else
        echo "$name: Stopped"
    fi
}

case "${1:-status}" in
    start)
        echo "Starting serial port forwarding..."
        start_forward "opalum" "$OPALUM_HOST" "$OPALUM_PORT" "$OPALUM_PTY"
        echo ""
        echo "Local devices available:"
        echo "  Opalum: $OPALUM_PTY"
        ;;
    stop)
        echo "Stopping serial port forwarding..."
        stop_forward "opalum"
        rm -f "$OPALUM_PTY" 2>/dev/null || true
        ;;
    status)
        echo "Serial port forwarding status:"
        status_forward "opalum" "$OPALUM_PTY"
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    *)
        echo "Usage: $0 {start|stop|status|restart}"
        exit 1
        ;;
esac

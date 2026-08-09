#!/bin/bash
# Run the broker tests: start the mock Denon, point the SocketClientBackend at
# the freshly built avr-controld (which it auto-spawns against the mock), run
# broker_tests, then shut the daemon down and stop the mock. No hardware needed.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

MOCK_HOST=127.0.0.1
MOCK_PORT="${MOCK_PORT:-23334}"
SOCKDIR="$(mktemp -d /tmp/avrsock.XXXXXX)"

export MOCK_HOST MOCK_PORT
export AVR_SOCKET_DIR="$SOCKDIR"
export AVR_CONTROLD_BIN="$BUILD_DIR/avr-controld"

# max-volume 98 -> near-exact percent<->MV round-trip in the tests
python3 "$ROOT/tests/mock_server/denon_mock_server.py" \
    --host "$MOCK_HOST" --port "$MOCK_PORT" --max-volume 98 &
MOCK_PID=$!

cleanup() {
    for s in "$SOCKDIR"/*.sock; do
        [ -S "$s" ] || continue
        python3 - "$s" <<'PY' 2>/dev/null || true
import socket, sys
try:
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(sys.argv[1]); c.sendall(b"SHUTDOWN\n"); c.recv(16)
except Exception:
    pass
PY
    done
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
    rm -rf "$SOCKDIR"
}
trap cleanup EXIT

for _ in $(seq 1 20); do
    if python3 -c "import socket;socket.create_connection(('$MOCK_HOST',$MOCK_PORT),1).close()" 2>/dev/null; then break; fi
    sleep 0.25
done

BIN="$BUILD_DIR/broker_tests"
if [ ! -x "$BIN" ]; then echo "broker_tests not built at $BIN"; exit 1; fi
"$BIN"

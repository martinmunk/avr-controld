#!/bin/bash
#
# Helper script to run integration tests with the mock Denon server
#
# Usage:
#   ./run_integration_tests.sh [options]
#
# Options:
#   --mock        Use mock server (default)
#   --real HOST   Use real Denon receiver at HOST
#   --port PORT   Port to use (default: 23 for mock, detect for real)
#   --max-volume  Max volume for mock server (default: 98)
#

set -e

# Default values
USE_MOCK=1
DENON_HOST="127.0.0.1"
DENON_PORT=23333  # Use non-privileged port for mock by default
MAX_VOLUME=98
BUILD_DIR="$(dirname "$0")/../build"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --mock)
            USE_MOCK=1
            shift
            ;;
        --real)
            USE_MOCK=0
            DENON_HOST="$2"
            DENON_PORT=23
            shift 2
            ;;
        --port)
            DENON_PORT="$2"
            shift 2
            ;;
        --max-volume)
            MAX_VOLUME="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Integration Test Runner${NC}"
echo -e "${GREEN}========================================${NC}"
echo

if [ $USE_MOCK -eq 1 ]; then
    echo -e "${YELLOW}Starting mock Denon server...${NC}"
    echo "  Host: $DENON_HOST"
    echo "  Port: $DENON_PORT"
    echo "  Max Volume: $MAX_VOLUME"
    echo

    # Start mock server in background
    python3 "$(dirname "$0")/mock_server/denon_mock_server.py" \
        --host "$DENON_HOST" \
        --port "$DENON_PORT" \
        --max-volume "$MAX_VOLUME" &
    MOCK_PID=$!

    # Wait for server to start
    echo "Waiting for server to start..."
    for i in {1..10}; do
        if timeout 1 bash -c "cat < /dev/null > /dev/tcp/$DENON_HOST/$DENON_PORT" 2>/dev/null; then
            echo -e "${GREEN}Mock server started (PID: $MOCK_PID)${NC}"
            echo
            break
        fi
        sleep 0.5
    done

    # Ensure cleanup on exit
    trap "echo 'Stopping mock server...'; kill $MOCK_PID 2>/dev/null || true; wait $MOCK_PID 2>/dev/null || true" EXIT
else
    echo -e "${YELLOW}Using real Denon receiver:${NC}"
    echo "  Host: $DENON_HOST"
    echo "  Port: $DENON_PORT"
    echo
    echo -e "${RED}WARNING: Tests will control your real receiver!${NC}"
    echo "Press Ctrl+C within 3 seconds to cancel..."
    sleep 3
    echo
fi

# Set environment variables for integration tests
export DENON_TEST_HOST="$DENON_HOST"
export DENON_TEST_PORT="$DENON_PORT"

# Run the integration tests
echo -e "${GREEN}Running integration tests...${NC}"
echo

cd "$BUILD_DIR"

if [ -f "./integration_tests" ]; then
    ./integration_tests
    TEST_RESULT=$?
else
    echo -e "${RED}Error: integration_tests executable not found${NC}"
    echo "Please build the tests first:"
    echo "  cd cpp/build && cmake .. && make integration_tests"
    exit 1
fi

echo
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  All integration tests passed!${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}  Some integration tests failed${NC}"
    echo -e "${RED}========================================${NC}"
fi

exit $TEST_RESULT

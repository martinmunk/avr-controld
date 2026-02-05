#!/bin/bash
#
# Run ALSA plugin tests (no Docker or system-wide install needed!)
#
# Usage:
#   ./run_alsa_tests.sh [pytest options]
#
# Examples:
#   ./run_alsa_tests.sh                    # Run all tests
#   ./run_alsa_tests.sh -k test_volume    # Run only volume tests
#   ./run_alsa_tests.sh -v -s             # Verbose with output
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  ALSA Plugin Test Runner${NC}"
echo -e "${GREEN}========================================${NC}"
echo

# Check if plugin is built
if [ ! -f "$BUILD_DIR/libasound_module_ctl_denon.so" ]; then
    echo -e "${RED}Error: Plugin not built${NC}"
    echo "Please build first:"
    echo "  cd $PROJECT_ROOT/build"
    echo "  cmake .."
    echo "  make"
    exit 1
fi

echo -e "${YELLOW}Plugin found: $BUILD_DIR/libasound_module_ctl_denon.so${NC}"
echo -e "${YELLOW}Using local ALSA config (no system install needed)${NC}"
echo

# Run pytest with local config
# ALSA_CONFIG_PATH is set in conftest.py
cd "$SCRIPT_DIR"
python3 -m pytest "$@"
TEST_RESULT=$?

echo
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  All ALSA plugin tests passed!${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}  Some ALSA plugin tests failed${NC}"
    echo -e "${RED}========================================${NC}"
fi

exit $TEST_RESULT

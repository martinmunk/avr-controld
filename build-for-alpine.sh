#!/bin/bash
#
# Build ALSA AVR control plugins for Alpine Linux
# Installs to /opt/alsa-avr-control on the host for volume mounting
#
# Usage:
#   ./build-for-alpine.sh              # Build and install
#   ./build-for-alpine.sh --clean      # Clean build directory
#

set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="/opt/alsa-avr-control"
BUILD_DIR="$SCRIPT_DIR/build-alpine"

# Handle --clean flag
if [ "$1" = "--clean" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
    echo -e "${GREEN}✓${NC} Build directory cleaned"
    exit 0
fi

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}Build ALSA AVR Control for Alpine${NC}"
echo -e "${BLUE}================================${NC}"
echo ""
echo -e "Source: ${SCRIPT_DIR}"
echo -e "Build:  ${BUILD_DIR}"
echo -e "Install: ${INSTALL_PREFIX}"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"

# Check if running on ARM or AMD64
ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    DOCKER_PLATFORM="linux/arm64"
    echo -e "Platform: ${GREEN}ARM64${NC}"
elif [ "$ARCH" = "x86_64" ]; then
    DOCKER_PLATFORM="linux/amd64"
    echo -e "Platform: ${GREEN}AMD64${NC}"
else
    echo -e "${YELLOW}Warning: Unknown architecture $ARCH, defaulting to linux/$ARCH${NC}"
    DOCKER_PLATFORM="linux/$ARCH"
fi
echo ""

# Build in Alpine container
echo -e "${GREEN}Building in Alpine container...${NC}"
docker run --rm \
    --platform "$DOCKER_PLATFORM" \
    -v "$SCRIPT_DIR:/src" \
    -v "$BUILD_DIR:/build" \
    alpine:latest \
    sh -c "
        set -e
        echo 'Installing build dependencies...'
        apk add --no-cache build-base cmake alsa-lib-dev >/dev/null 2>&1

        echo 'Configuring CMake...'
        cd /build
        cmake /src \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/opt/alsa-avr-control \
            -DBUILD_TESTS=OFF

        echo 'Building...'
        make -j\$(nproc)

        echo 'Installing to staging directory...'
        make install DESTDIR=/build/install

        echo 'Build complete!'
    "

echo ""
echo -e "${GREEN}✓${NC} Build complete"
echo ""

# Create install directory with sudo
echo -e "${GREEN}Installing to ${INSTALL_PREFIX}...${NC}"
sudo mkdir -p "$INSTALL_PREFIX"

# Copy files from staging to actual install location
sudo cp -rv "$BUILD_DIR/install/opt/alsa-avr-control/"* "$INSTALL_PREFIX/"

echo ""
echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}Installation complete!${NC}"
echo -e "${GREEN}================================${NC}"
echo ""
echo "Plugins installed to:"
ls -lh "$INSTALL_PREFIX/lib/alsa-lib/"
echo ""
echo "Configuration files:"
ls -lh "$INSTALL_PREFIX/etc/alsa/conf.d/" 2>/dev/null || echo "(No config files)"
echo ""
echo "To use in Docker, add volume mounts:"
echo "  -v $INSTALL_PREFIX/lib/alsa-lib:/usr/lib/alsa-lib:ro"
echo "  -v $INSTALL_PREFIX/etc/alsa/conf.d:/etc/alsa/conf.d:ro"
echo ""
echo "Or in docker-compose.yml:"
echo "  volumes:"
echo "    - $INSTALL_PREFIX/lib/alsa-lib:/usr/lib/alsa-lib:ro"
echo "    - $INSTALL_PREFIX/etc/alsa/conf.d:/etc/alsa/conf.d:ro"
echo ""

#!/bin/bash
#
# Build Debian package for martinmunk-alsa-avr-pi
#
# Usage:
#   ./buildpackage.sh                    # Build for current architecture
#   TARGET_ARCH=arm64 ./buildpackage.sh  # Cross-compile for arm64
#   TARGET_ARCH=armhf ./buildpackage.sh  # Cross-compile for armhf

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Detect target architecture (default to current architecture)
TARGET_ARCH=${TARGET_ARCH:-$(dpkg --print-architecture)}
CURRENT_ARCH=$(dpkg --print-architecture)

echo "Building for architecture: ${TARGET_ARCH}"

# Check if build dependencies are satisfied
if ! dpkg-checkbuilddeps 2>/dev/null; then
    echo "Build dependencies are not satisfied."

    if command -v mk-build-deps &> /dev/null; then
        if [ "$EUID" -eq 0 ] || [ -n "$SUDO_USER" ]; then
            # Running with sudo or as root
            echo "Installing build dependencies..."
            mk-build-deps -i -r -t "apt-get -y" debian/control 2>/dev/null || true
        else
            # Not running with sudo, prompt user
            read -p "Install build dependencies using sudo? (y/N): " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                sudo mk-build-deps -i -r -t "apt-get -y" debian/control
            else
                echo "Warning: Build may fail due to missing dependencies"
                echo "To install manually: sudo mk-build-deps -i -r debian/control"
            fi
        fi
    else
        echo "Warning: mk-build-deps not found. Install with: sudo apt-get install devscripts equivs"
        echo "Or manually install dependencies listed in debian/control"
    fi
fi

mkdir -p build-deb
rm -rf build-deb/*
rsync -a --exclude build --exclude build-deb --exclude build-arm64 --exclude build-alpine-arm64 \
         --exclude '*.deb' --exclude '*.changes' --exclude '*.buildinfo' \
         --exclude debian-arm64-out --exclude alpine-arm64-out \
         --delete . build-deb/source

# Determine distribution suffix (e.g., debian10buster, ubuntu2204jammy)
if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "$ID" in
        debian)
            case "$VERSION_ID" in
                10) DIST_SUFFIX="debian10buster" ;;
                11) DIST_SUFFIX="debian11bullseye" ;;
                12) DIST_SUFFIX="debian12bookworm" ;;
                13) DIST_SUFFIX="debian13trixie" ;;
                *) DIST_SUFFIX="debian${VERSION_ID}${VERSION_CODENAME}" ;;
            esac
            ;;
        ubuntu)
            DIST_SUFFIX="ubuntu${VERSION_ID//.}${VERSION_CODENAME}"
            ;;
        *)
            DIST_SUFFIX="${ID}${VERSION_ID}"
            ;;
    esac

    # Update changelog with distribution-specific version
    cd build-deb/source
    ORIGINAL_VERSION=$(dpkg-parsechangelog -S Version)
    NEW_VERSION="${ORIGINAL_VERSION}+${DIST_SUFFIX}"
    echo "Building version ${NEW_VERSION} for ${DIST_SUFFIX}"
    dch -b -v "${NEW_VERSION}" -D stable "Build for ${DIST_SUFFIX}" --force-distribution
    cd ../..
fi

# Build package
cd build-deb/source
if [ "$TARGET_ARCH" != "$CURRENT_ARCH" ]; then
    echo "Cross-compiling for ${TARGET_ARCH}"
    export DEB_BUILD_OPTIONS="nocheck"
    export DEB_BUILD_PROFILES="cross nocheck"
    dpkg-buildpackage -a${TARGET_ARCH} --no-sign -rfakeroot
else
    echo "Building natively for ${TARGET_ARCH}"
    dpkg-buildpackage --no-sign -rfakeroot
fi
cd ../..

echo "Build complete. Packages are in build-deb/"
ls -lh build-deb/*.deb 2>/dev/null || echo "No .deb files found"

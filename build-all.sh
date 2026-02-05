#!/bin/bash
# Build all variants of the ALSA AVR plugins

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "================================"
echo "Building all ALSA AVR plugins"
echo "================================"

# Build locally for x86_64
echo ""
echo "==> Building for local x86_64..."
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cd ..
echo "✓ x86_64 build complete"

# Build for ARM64 Debian (glibc)
echo ""
echo "==> Cross-compiling for ARM64 Debian (glibc)..."
sg docker -c "docker build --no-cache -f Dockerfile.cross-compile -t alsa-avr-cross:latest ."
mkdir -p build-arm64
sg docker -c "docker run --rm -v \"\$PWD/build-arm64:/output\" alsa-avr-cross:latest"
echo "✓ ARM64 Debian (glibc) build complete"

# Build for ARM64 Alpine (musl)
echo ""
echo "==> Cross-compiling for ARM64 Alpine (musl)..."
sg docker -c "docker build --no-cache -f Dockerfile.cross-compile-alpine -t alsa-avr-cross-alpine:latest ."
mkdir -p build-alpine-arm64
sg docker -c "docker run --rm -v \"\$PWD/build-alpine-arm64:/output\" alsa-avr-cross-alpine:latest"
echo "✓ ARM64 Alpine (musl) build complete"

echo ""
echo "================================"
echo "All builds completed successfully!"
echo "================================"
echo ""
echo "Built artifacts:"
echo "  x86_64:           build/libasound_module_ctl_*.so"
echo "  ARM64 Debian:     build-arm64/libasound_module_ctl_*.so"
echo "  ARM64 Alpine:     build-alpine-arm64/libasound_module_ctl_*.so"
echo ""

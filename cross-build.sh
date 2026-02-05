#!/bin/bash
# Cross-compile ALSA AVR plugin for ARM64 and deploy to Raspberry Pi

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
OUTPUT_DIR="$SCRIPT_DIR/build-arm64"
PI_HOST="${1:-192.168.20.116}"
PI_USER="${2:-martinmunk}"

echo "==> Building ARM64 cross-compilation container..."
sg docker -c "docker build --no-cache -f \"$SCRIPT_DIR/Dockerfile.cross-compile\" -t alsa-avr-cross:latest \"$SCRIPT_DIR\""

echo "==> Cross-compiling for ARM64..."
mkdir -p "$OUTPUT_DIR"
sg docker -c "docker run --rm -v \"$OUTPUT_DIR:/output\" alsa-avr-cross:latest"

echo "==> Built libraries:"
ls -lh "$OUTPUT_DIR"/libasound_module_ctl_*.so
file "$OUTPUT_DIR"/libasound_module_ctl_*.so

echo "==> Deploying to Raspberry Pi at $PI_USER@$PI_HOST (host system)..."
scp "$OUTPUT_DIR"/libasound_module_ctl_*.so "$PI_USER@$PI_HOST:/tmp/"
ssh "$PI_USER@$PI_HOST" "sudo mkdir -p /usr/local/lib/alsa-lib && sudo cp /tmp/libasound_module_ctl_*.so /usr/local/lib/alsa-lib/ && sudo rm /tmp/libasound_module_ctl_*.so"

echo "==> Restarting shairport-sync service on Pi..."
ssh "$PI_USER@$PI_HOST" "sudo systemctl restart shairport-sync-docker.service"

echo "==> Done! Plugin deployed and service restarted."

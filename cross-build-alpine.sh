#!/bin/bash
# Cross-compile ALSA AVR plugin for ARM64 (Alpine/musl) and deploy to Raspberry Pi

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
OUTPUT_DIR="$SCRIPT_DIR/build-alpine-arm64"
PI_HOST="${1:-192.168.20.116}"
PI_USER="${2:-martinmunk}"

echo "==> Building Alpine ARM64 cross-compilation container..."
sg docker -c "docker build -f \"$SCRIPT_DIR/Dockerfile.cross-compile-alpine\" -t alsa-avr-cross-alpine:latest \"$SCRIPT_DIR\""

echo "==> Cross-compiling for ARM64 (musl)..."
mkdir -p "$OUTPUT_DIR"
sg docker -c "docker run --rm -v \"$OUTPUT_DIR:/output\" alsa-avr-cross-alpine:latest"

echo "==> Built libraries and tools:"
ls -lh "$OUTPUT_DIR"/libasound_module_ctl_*.so "$OUTPUT_DIR"/avr-power 2>/dev/null || ls -lh "$OUTPUT_DIR"/
file "$OUTPUT_DIR"/libasound_module_ctl_*.so "$OUTPUT_DIR"/avr-power 2>/dev/null || true

echo "==> Deploying to Raspberry Pi at $PI_USER@$PI_HOST (Docker plugin directory)..."
scp "$OUTPUT_DIR"/libasound_module_ctl_*.so "$OUTPUT_DIR"/avr-power "$PI_USER@$PI_HOST:/tmp/"
ssh "$PI_USER@$PI_HOST" "sudo mkdir -p /usr/local/lib/alsa-lib-docker && sudo cp /tmp/libasound_module_ctl_*.so /usr/local/lib/alsa-lib-docker/ && sudo rm /tmp/libasound_module_ctl_*.so"
ssh "$PI_USER@$PI_HOST" "sudo cp /tmp/avr-power /usr/local/bin/ && sudo chmod +x /usr/local/bin/avr-power && sudo rm /tmp/avr-power"
ssh "$PI_USER@$PI_HOST" "sudo ln -sf /usr/local/bin/avr-power /usr/local/bin/avr-power-on && sudo ln -sf /usr/local/bin/avr-power /usr/local/bin/avr-power-off"

echo "==> Restarting shairport-sync service on Pi..."
ssh "$PI_USER@$PI_HOST" "sudo systemctl restart shairport-sync-docker.service"

echo "==> Done! Plugin deployed and service restarted."

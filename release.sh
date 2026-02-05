#!/bin/bash
# Release ALSA AVR plugins to Raspberry Pis
# This script deploys both Debian (glibc) and Alpine (musl) versions to both Pis

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

PI116_HOST="${1:-martinmunk@192.168.20.116}"
PI240_HOST="${2:-martinmunk@192.168.20.240}"

echo "================================"
echo "Releasing ALSA AVR plugins"
echo "================================"
echo ""
echo "Target hosts:"
echo "  Pi 116 (Opalum):  $PI116_HOST"
echo "  Pi 240 (Denon):   $PI240_HOST"
echo ""

# Check that builds exist
if [ ! -d "build-arm64" ] || [ ! -d "build-alpine-arm64" ]; then
    echo "ERROR: Build directories not found. Please run ./build-all.sh first."
    exit 1
fi

# Function to deploy to a Pi
deploy_to_pi() {
    local HOST=$1
    local NAME=$2

    echo "==> Deploying to $NAME ($HOST)..."

    # Copy ARM64 Debian (glibc) plugins for host
    echo "  - Copying glibc plugins (host system)..."
    scp build-arm64/libasound_module_ctl_*.so "$HOST:/tmp/"

    # Copy ARM64 Alpine (musl) plugins for Docker
    echo "  - Copying musl plugins (Docker container)..."
    for f in build-alpine-arm64/libasound_module_ctl_*.so; do
        basename_f=$(basename "$f")
        scp "$f" "$HOST:/tmp/musl-$basename_f"
    done

    # Install glibc plugins on host (all standard ALSA lib locations)
    echo "  - Installing glibc plugins on host..."
    ssh "$HOST" "sudo cp /tmp/libasound_module_ctl_*.so /usr/lib/alsa-lib/ 2>/dev/null && echo '    ✓ Installed to /usr/lib/alsa-lib/' || true"
    ssh "$HOST" "sudo cp /tmp/libasound_module_ctl_*.so /usr/lib/aarch64-linux-gnu/alsa-lib/ 2>/dev/null && echo '    ✓ Installed to /usr/lib/aarch64-linux-gnu/alsa-lib/' || true"

    # Install musl plugins for Docker
    echo "  - Installing musl plugins for Docker..."
    ssh "$HOST" "cd /tmp && for f in musl-libasound_module_ctl_*.so; do sudo mkdir -p /usr/local/lib/alsa-lib-docker && sudo cp \"\$f\" /usr/local/lib/alsa-lib-docker/\"\${f#musl-}\"; done && echo '    ✓ Installed musl plugins to /usr/local/lib/alsa-lib-docker/'"

    # Clean up temp files
    ssh "$HOST" "rm -f /tmp/musl-*.so /tmp/libasound_module_ctl_*.so"

    # Restart Docker service
    echo "  - Restarting shairport-sync Docker service..."
    ssh "$HOST" "sudo systemctl restart shairport-sync-docker.service"

    echo "✓ $NAME deployment complete"
    echo ""
}

# Deploy to both Pis
deploy_to_pi "$PI116_HOST" "Pi 116 (VærkstedsTærten)"
deploy_to_pi "$PI240_HOST" "Pi 240 (StueTærten)"

echo "================================"
echo "Release completed successfully!"
echo "================================"
echo ""
echo "Testing deployment..."
echo ""

# Test Pi 116 (Opalum)
echo "==> Testing Pi 116 (Opalum):"
ssh "$PI116_HOST" "amixer -D opalum scontrols 2>&1" || echo "  (Note: Test may fail if serial device is not connected)"
echo ""

# Test Pi 240 (Denon)
echo "==> Testing Pi 240 (Denon):"
ssh "$PI240_HOST" "amixer -D denon scontrols 2>&1" || echo "  (Note: Test may fail if Denon receiver is powered off)"
echo ""

echo "Done! Both Pis have been updated with the new plugins."

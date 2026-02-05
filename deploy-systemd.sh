#!/bin/bash
# Deploy systemd service configuration to Raspberry Pis

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Check for -y flag
SKIP_CONFIRM=0
PI116_HOST="martinmunk@192.168.20.116"
PI240_HOST="martinmunk@192.168.20.240"

for arg in "$@"; do
    if [[ "$arg" == "-y" ]]; then
        SKIP_CONFIRM=1
    fi
done

# Parse host arguments (if not -y)
for arg in "$@"; do
    if [[ "$arg" != "-y" ]]; then
        if [[ -z "$PI116_SET" ]]; then
            PI116_HOST="$arg"
            PI116_SET=1
        elif [[ -z "$PI240_SET" ]]; then
            PI240_HOST="$arg"
            PI240_SET=1
        fi
    fi
done

echo "================================"
echo "Deploying Systemd Configuration"
echo "================================"
echo ""
echo "Target hosts:"
echo "  Pi 116 (Opalum):  $PI116_HOST"
echo "  Pi 240 (Denon):   $PI240_HOST"
echo ""

# Function to deploy systemd config to a Pi
deploy_systemd_to_pi() {
    local HOST=$1
    local NAME=$2
    local CONFIG_FILE=$3

    echo "==> Deploying to $NAME ($HOST)..."

    # Copy systemd service file
    echo "  - Copying systemd service file..."
    scp systemd/shairport-sync-avr.service "$HOST:/tmp/"
    ssh "$HOST" "sudo cp /tmp/shairport-sync-avr.service /etc/systemd/system/shairport-sync-docker.service && rm /tmp/shairport-sync-avr.service"

    # Create config directory
    echo "  - Creating /etc/shairport-sync-avr/ directory..."
    ssh "$HOST" "sudo mkdir -p /etc/shairport-sync-avr"

    # Install ALSA conf.d files (system-wide plugin definitions)
    echo "  - Installing ALSA conf.d files..."
    ssh "$HOST" "sudo mkdir -p /etc/alsa/conf.d"
    scp examples/conf.d/50-denon-avr.conf "$HOST:/tmp/"
    scp examples/conf.d/50-nad-serial.conf "$HOST:/tmp/"
    scp examples/conf.d/50-opalum-serial.conf "$HOST:/tmp/"
    scp examples/conf.d/50-cec-control.conf "$HOST:/tmp/"
    ssh "$HOST" "sudo cp /tmp/50-*.conf /etc/alsa/conf.d/ && rm /tmp/50-*.conf"
    echo "    ✓ Installed conf.d files (denon, nad, opalum, cec)"

    # Remove any old asound.conf files
    echo "  - Cleaning up old asound.conf files..."
    ssh "$HOST" "sudo rm -f /etc/asound.conf /etc/shairport-sync-avr/asound.conf"
    echo "    ✓ Removed old asound.conf files"

    # Install per-Pi local configuration in conf.d
    echo "  - Installing per-Pi local conf.d configuration..."
    if [ "$NAME" = "Pi 116 (VærkstedsTærten)" ]; then
        scp "systemd/90-local-config.conf.pi116-opalum" "$HOST:/tmp/90-local-config.conf"
    else
        scp "systemd/90-local-config.conf.pi240-denon" "$HOST:/tmp/90-local-config.conf"
    fi
    ssh "$HOST" "sudo cp /tmp/90-local-config.conf /etc/alsa/conf.d/ && rm /tmp/90-local-config.conf"
    echo "    ✓ Installed local configuration to /etc/alsa/conf.d/90-local-config.conf"

    # Copy service configuration (only if it doesn't exist)
    echo "  - Checking service configuration..."
    if ssh "$HOST" "[ ! -f /etc/shairport-sync-avr/service.conf ]"; then
        echo "    → service.conf not found, installing default..."
        scp "systemd/$CONFIG_FILE" "$HOST:/tmp/service.conf"
        ssh "$HOST" "sudo cp /tmp/service.conf /etc/shairport-sync-avr/service.conf && rm /tmp/service.conf"
        echo "    ✓ Installed default service.conf"
    else
        echo "    ℹ service.conf already exists, keeping current configuration"
        echo "    (To update, manually copy systemd/$CONFIG_FILE to the Pi)"
    fi

    # Reload systemd
    echo "  - Reloading systemd..."
    ssh "$HOST" "sudo systemctl daemon-reload"

    # Check if service is enabled
    if ssh "$HOST" "systemctl is-enabled shairport-sync-docker.service >/dev/null 2>&1"; then
        echo "  - Service is enabled"
        # Restart if running
        if ssh "$HOST" "systemctl is-active shairport-sync-docker.service >/dev/null 2>&1"; then
            echo "  - Restarting service..."
            ssh "$HOST" "sudo systemctl restart shairport-sync-docker.service"
        else
            echo "  - Service is not running, starting..."
            ssh "$HOST" "sudo systemctl start shairport-sync-docker.service"
        fi
    else
        echo "  - Service is not enabled. Enable with:"
        echo "    ssh $HOST sudo systemctl enable shairport-sync-docker.service"
        echo "    ssh $HOST sudo systemctl start shairport-sync-docker.service"
    fi

    echo "✓ $NAME systemd deployment complete"
    echo ""
}

if [ $SKIP_CONFIRM -eq 0 ]; then
    # Ask for confirmation
    echo "This will update the systemd service files on both Pis."
    echo "Existing /etc/shairport-sync-avr/service.conf files will be preserved."
    echo ""
    read -p "Continue? (y/N) " -n 1 -r
    echo ""

    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 0
    fi

    echo ""
fi

# Deploy to both Pis
deploy_systemd_to_pi "$PI116_HOST" "Pi 116 (VærkstedsTærten)" "service.conf.pi116-opalum"
deploy_systemd_to_pi "$PI240_HOST" "Pi 240 (StueTærten)" "service.conf.pi240-denon"

echo "================================"
echo "Systemd deployment completed!"
echo "================================"
echo ""
echo "Current service status:"
echo ""

# Show service status
echo "==> Pi 116 status:"
ssh "$PI116_HOST" "systemctl status shairport-sync-docker.service --no-pager -l" || true
echo ""

echo "==> Pi 240 status:"
ssh "$PI240_HOST" "systemctl status shairport-sync-docker.service --no-pager -l" || true
echo ""

echo "Done! You can check logs with:"
echo "  ssh $PI116_HOST sudo journalctl -u shairport-sync-docker.service -f"
echo "  ssh $PI240_HOST sudo journalctl -u shairport-sync-docker.service -f"

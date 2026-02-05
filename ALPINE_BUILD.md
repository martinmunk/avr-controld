# Building for Alpine Linux

This guide explains how to use Alpine-built packages (musl-linked) alongside native Debian packages on your Raspberry Pi.

## Why Alpine Build?

The shairport-sync Docker image is based on Alpine Linux, which uses **musl libc** instead of **glibc** (used by Debian). Debian packages compiled for glibc won't work in Alpine containers.

## Quick Start

### 1. Install Alpine-Built Package from Repository

Add the Alpine repository and install:

```bash
echo 'deb http://gitea.servers.vejen.loc.martinmunk.com:3000/martinmunk/debian stable main' | \
  sudo tee /etc/apt/sources.list.d/martinmunk-alpine.list
sudo apt update
sudo apt install martinmunk-alsa-control-avr-alpine
```

This installs musl-linked binaries to `/opt/alsa-avr-control-alpine/`

### 2. Run Container with Volume Mounts

Use the example docker-compose configuration:

```bash
cp docker-compose.example.yml docker-compose.yml
docker-compose up -d
```

Or run directly with docker:

```bash
docker run -d \
  --name shairport-sync \
  --restart unless-stopped \
  --network host \
  --device /dev/snd \
  --cap-add SYS_NICE \
  -v /opt/alsa-avr-control-alpine/lib/alsa-lib:/usr/lib/alsa-lib:ro \
  -v /opt/alsa-avr-control-alpine/etc/alsa/conf.d:/etc/alsa/conf.d:ro \
  mikebrady/shairport-sync:latest
```

### 3. Verify Installation

Check that plugins are available inside the container:

```bash
docker exec shairport-sync ls -l /usr/lib/alsa-lib/
```

You should see:
- `libasound_module_ctl_denon.so`
- `libasound_module_ctl_nad.so`
- `libasound_module_ctl_opalum.so`
- `libasound_module_ctl_cec.so`

## Benefits of This Approach

1. **No image rebuild needed** - Update plugins without rebuilding Docker image
2. **Faster container startup** - No build step in Dockerfile
3. **Easy updates** - Just `apt upgrade` and restart container
4. **Both packages together** - Native and Alpine packages can coexist

## Both Packages on the Same Pi

You can install both packages simultaneously:

```bash
# Native Debian package (glibc) - standard paths
sudo apt install martinmunk-alsa-control-avr

# Alpine package (musl) - /opt/alsa-avr-control-alpine/
sudo apt install martinmunk-alsa-control-avr-alpine
```

- **Native package** (`martinmunk-alsa-control-avr`): Use with ALSA on the host
- **Alpine package** (`martinmunk-alsa-control-avr-alpine`): Volume mount into Alpine containers

## Updating Plugins

When a new version is released:

```bash
# Update package from repository
sudo apt update
sudo apt upgrade martinmunk-alsa-control-avr-alpine

# Restart container to use new plugins
docker-compose restart shairport-sync
```

## Directory Structure

After installing `martinmunk-alsa-control-avr-alpine`:

```
/opt/alsa-avr-control-alpine/
├── lib/
│   └── alsa-lib/
│       ├── libasound_module_ctl_denon.so  (musl-linked)
│       ├── libasound_module_ctl_nad.so
│       ├── libasound_module_ctl_opalum.so
│       └── libasound_module_ctl_cec.so
├── etc/
│   └── alsa/
│       └── conf.d/
│           ├── 50-denon-avr.conf
│           ├── 50-nad-serial.conf
│           ├── 50-opalum-serial.conf
│           └── 50-cec-control.conf
└── bin/
    └── avr-power
```

After installing `martinmunk-alsa-control-avr` (native):

```
/usr/
├── lib/alsa-lib/               (glibc-linked)
├── etc/alsa/conf.d/
└── bin/avr-power
```

## Troubleshooting

### Plugins not found in container

Check volume mounts:
```bash
docker inspect shairport-sync | grep -A 10 Mounts
```

### Permission issues

The build script uses sudo to install to `/opt/alsa-avr-control/`. Make sure you have sudo access.

### Architecture mismatch

The script auto-detects architecture. If you need to force a specific arch:
```bash
docker run --platform linux/arm64 ...  # For ARM64
docker run --platform linux/amd64 ...  # For AMD64
```

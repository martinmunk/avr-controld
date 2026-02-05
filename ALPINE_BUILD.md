# Building for Alpine Linux

This guide explains how to build the ALSA AVR control plugins for Alpine Linux (used by shairport-sync container) and mount them into your container.

## Why Alpine Build?

The shairport-sync Docker image is based on Alpine Linux, which uses **musl libc** instead of **glibc** (used by Debian). Debian packages compiled for glibc won't work in Alpine containers.

## Quick Start

### 1. Build for Alpine

Run the build script on your Raspberry Pi:

```bash
./build-for-alpine.sh
```

This will:
- Build the plugins inside an Alpine container (ensuring musl compatibility)
- Install to `/opt/alsa-avr-control/` on your Pi
- Auto-detect your architecture (ARM64 or AMD64)

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
  -v /opt/alsa-avr-control/lib/alsa-lib:/usr/lib/alsa-lib:ro \
  -v /opt/alsa-avr-control/etc/alsa/conf.d:/etc/alsa/conf.d:ro \
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
3. **Easy updates** - Just rerun `./build-for-alpine.sh` and restart container
4. **Correct architecture** - Auto-detects ARM64/AMD64

## Alternative: Build Into Image

If you prefer the original approach of building plugins into the Docker image:

```bash
docker build -f Dockerfile.shairport -t shairport-sync-custom .
```

Then run the `shairport-sync-custom` service in docker-compose.

## Updating Plugins

When you update the plugin source code:

```bash
# Rebuild for Alpine
./build-for-alpine.sh

# Restart container to use new plugins
docker-compose restart shairport-sync
```

## Directory Structure

After running `build-for-alpine.sh`:

```
/opt/alsa-avr-control/
├── lib/
│   └── alsa-lib/
│       ├── libasound_module_ctl_denon.so
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

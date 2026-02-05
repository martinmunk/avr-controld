# Level 3 Testing - Docker Environment Instructions

## Status Summary

### ✅ Completed:
- **Level 1: Unit Tests** - 70 tests passing
  - `tests/unit/test_denon_protocol.cpp` - Protocol parsing (30 tests)
  - `tests/unit/test_volume_scaling.cpp` - Volume conversion (40 tests)
  - Run with: `./build/unit_tests`

- **Level 2: Integration Tests** - 25 tests passing on real hardware
  - `tests/integration/test_denon_connection.cpp` - Backend testing
  - Tested successfully with real Denon AVR at 192.168.20.216
  - Run with: `DENON_TEST_HOST=192.168.20.216 DENON_TEST_PORT=23 ./build/integration_tests`

### 🔄 In Progress:
- **Level 3: ALSA Plugin Tests** - pytest-based end-to-end tests

## What Needs to Happen Inside Docker

When relaunched inside a Docker container, follow these steps:

### 1. Build the Plugin
```bash
cd /src  # Or wherever source is mounted
mkdir -p build && cd build
cmake -DBUILD_TESTS=OFF ..
make
```

### 2. Install Plugin
```bash
# Copy plugin to ALSA lib directory
cp libasound_module_ctl_denon.so /usr/lib/x86_64-linux-gnu/alsa-lib/

# Or if different architecture
cp libasound_module_ctl_denon.so /usr/lib/alsa-lib/
```

### 3. Configure ALSA
The config file is at `tests/alsa_plugin/test-asound.conf`. It needs:
- Plugin type definition pointing to the .so file
- Control definition for "denon"
- Default control pointing to "denon"

Either:
- Copy to `/etc/asound.conf`
- Or set `ALSA_CONFIG_PATH=/path/to/test-asound.conf`

**Important**: Update the `lib` path in test-asound.conf to match where plugin was installed!

### 4. Start Mock Denon Server
```bash
cd tests/mock_server
python3 denon_mock_server.py --host 127.0.0.1 --port 23333 --max-volume 70 &
MOCK_PID=$!
```

### 5. Verify Plugin Loads
```bash
# This should list Master Playback Volume, Master Playback Switch, Master Playback Power controls
amixer -D default controls
# Or
amixer -c denon controls
```

### 6. Run Pytest Tests
```bash
cd tests/alsa_plugin
python3 -m pytest -v
```

## Test Files Ready

All test files are in `tests/alsa_plugin/`:
- `conftest.py` - Pytest fixtures and setup
  - **NOTE**: Has Docker detection via `DENON_HOST` env var
  - If `DENON_HOST=mock-denon`, assumes running in Docker and won't start local mock server
  - Otherwise starts mock server as subprocess

- `test_plugin_loading.py` - Plugin discovery and controls (7 tests)
- `test_volume_control.py` - Volume operations (7 tests)
- `test_mute_control.py` - Mute operations (4 tests)
- `test_concurrent_access.py` - Multi-client access (3 tests)

**Total: 21 pytest tests**

## Mock Server Details

File: `tests/mock_server/denon_mock_server.py`

Features:
- Full Denon IP protocol implementation
- Volume, mute, power, input control
- Configurable max volume (default 98)
- Multi-client support
- Command/response logging

Default behavior:
- Starts on port 23 (or --port specified)
- Max volume 98 (or --max-volume specified)
- Binds to 127.0.0.1 or 0.0.0.0 (--host specified)

## Potential Issues & Solutions

### Issue: conftest.py tries to start mock server automatically
**Solution**: Set environment variable before running pytest:
```bash
export DENON_HOST=mock-denon
export DENON_PORT=23333
```
This tells conftest.py to skip starting its own mock server.

### Issue: amixer says "Invalid card number"
**Solutions**:
1. Try `-D default` instead of `-c default`
2. Check `/etc/asound.conf` or `~/.asoundrc` exists and is valid
3. Verify plugin .so is in correct directory
4. Check `ctl_type.denon` section has correct `lib` path

### Issue: Plugin fails to load
**Check**:
1. Plugin built for correct architecture (x86_64 vs aarch64)
2. All dependencies available (libasound2, correct GLIBC version)
3. Run `ldd /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_ctl_denon.so`

### Issue: Tests can't reach mock server
**Check**:
1. Mock server is running: `nc -z 127.0.0.1 23333`
2. Firewall not blocking
3. Host/port match in ALSA config and test expectations

## Changes Made to Original Denon Backend

During Level 2 testing, the following change was made per user request:

**Input Auto-Switch**: Modified to ONLY switch input on first connection (during `init()`), NOT on volume changes or mute operations. The cooldown timer logic was removed.

File: `src/backends/denon_ip.cpp`
- Removed auto-switch from `set_volume_absolute()`
- Removed auto-switch from `set_mute()`
- Kept auto-switch only in `init()` if `input` config parameter is set

## Expected Test Results

When working correctly:
- All 21 pytest tests should PASS
- Mock server should show command/response logs
- Tests should complete in ~5-15 seconds
- No ALSA errors in output

## Git Status

Last commit included:
- All unit tests (Level 1)
- All integration tests (Level 2)
- Input auto-switch fix
- Mock server implementation

Clean docker directory (removed after failed approach).

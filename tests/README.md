# ALSA AVR Plugin Testing Framework

This directory contains a comprehensive multi-level testing framework for the ALSA AVR plugin.

## Test Levels

### Level 1: Unit Tests (No Hardware Required)
Unit tests for protocol parsing, volume scaling, and state management logic.

**Location:** `tests/unit/`

**Run:**
```bash
cd build
make unit_tests
./unit_tests
```

**Tests:**
- `test_denon_protocol.cpp` - Denon protocol message parsing
  - Volume response parsing (MV45, MV385, MVMAX detection)
  - Mute/power/input response parsing
  - Command formatting
  - Real-world protocol scenarios

- `test_volume_scaling.cpp` - Volume scaling and conversion
  - 0-100% ↔ Denon scale (0-98) conversion
  - Custom max volume support (e.g., max=70)
  - dB calculations for ALSA TLV
  - Round-trip conversions

### Level 2: Integration Tests (Hardware or Mock Server)
Integration tests for actual network/serial communication with backends.

**Location:** `tests/integration/`

**Run with Mock Server:**
```bash
cd tests
./run_integration_tests.sh --mock --port 23333
```

**Run with Real Hardware:**
```bash
cd tests
./run_integration_tests.sh --real 192.168.1.100 --port 23
```

**Tests:**
- Connection management and pooling
- Volume/mute/power/input control
- State querying and verification
- Error handling and reconnection
- Concurrent client access

### Level 3: ALSA Plugin Tests (pytest)
End-to-end tests for ALSA plugin integration using pytest and amixer.

**Requirements:** Plugin must be installed OR run inside Docker environment

**Location:** `tests/alsa_plugin/`

**Option A: System-wide install**
```bash
cd build
sudo make install

# Then run tests
cd ../tests/alsa_plugin
./run_alsa_tests.sh
```

**Option B: Inside Docker (recommended)**

When running inside a Docker container with proper ALSA setup:
1. Build plugin inside container
2. Install to `/usr/lib/x86_64-linux-gnu/alsa-lib/` or `/usr/lib/alsa-lib/`
3. Configure ALSA (update paths in `tests/alsa_plugin/test-asound.conf`)
4. Start mock server: `python3 tests/mock_server/denon_mock_server.py --port 23333 &`
5. Run tests: `cd tests/alsa_plugin && pytest -v`

See `LEVEL3_DOCKER_MEMO.md` for detailed Docker instructions.

**Tests (21 total):**
- Plugin loading via ALSA (7 tests)
- Volume control via `amixer` (7 tests)
- Mute control (4 tests)
- Concurrent access (3 tests)

## Mock Denon Server

A Python-based mock Denon AVR server for testing without hardware.

**Location:** `tests/mock_server/denon_mock_server.py`

**Features:**
- Full Denon IP protocol support
- Volume, mute, power, input control
- Configurable max volume
- Multi-client support
- Command/response logging

**Usage:**
```bash
# Default (port 23, max volume 98)
python3 tests/mock_server/denon_mock_server.py

# Custom port and max volume
python3 tests/mock_server/denon_mock_server.py --port 23333 --max-volume 70

# Bind to specific interface
python3 tests/mock_server/denon_mock_server.py --host 0.0.0.0 --port 23333
```

**Manual Testing:**
```bash
# Start server
python3 tests/mock_server/denon_mock_server.py --port 23333

# In another terminal, connect with telnet
telnet localhost 23333

# Send commands:
MV?        # Query volume
MV50       # Set volume to 50
MVUP       # Volume up
MUON       # Mute on
PW?        # Query power
SIMPLAY    # Set input to Media Player
```

## Running All Tests

```bash
# Build everything
cd cpp/build
cmake ..
make

# Run unit tests (fast, no hardware needed)
./unit_tests

# Run integration tests with mock server
cd ../tests
./run_integration_tests.sh --mock --port 23333

# Run ALSA plugin tests (requires Docker or root)
cd alsa_plugin
./run_alsa_tests.sh

# Run full Docker E2E tests
cd ../docker
docker-compose up
```

## Test Output

All test executables use Google Test framework and produce standard output:
- Green checkmarks for passing tests
- Red X for failing tests
- Summary with pass/fail counts
- Detailed failure messages

## CI/CD Integration

These tests are designed to run in CI/CD pipelines:

```yaml
# Example GitHub Actions workflow
- name: Build and Test
  run: |
    cd cpp/build
    cmake ..
    make unit_tests integration_tests

- name: Run Unit Tests
  run: cd cpp/build && ./unit_tests

- name: Run Integration Tests (Mock)
  run: |
    cd cpp/tests
    ./run_integration_tests.sh --mock --port 23333

- name: Run ALSA Plugin Tests
  run: |
    cd cpp/tests/alsa_plugin
    docker-compose up --abort-on-container-exit
```

## Test Development Guidelines

### Adding Unit Tests

1. Create test file in `tests/unit/test_<feature>.cpp`
2. Include gtest/gmock headers
3. Create test fixture class inheriting from `::testing::Test`
4. Write tests using `TEST_F(FixtureName, TestName)`
5. Add to CMakeLists.txt unit_tests target

### Adding Integration Tests

1. Add test cases to `tests/integration/test_denon_connection.cpp`
2. Use environment variables for configuration (DENON_TEST_HOST, etc.)
3. Skip tests gracefully if hardware not available
4. Always restore original state after tests

### Adding ALSA Plugin Tests

1. Create test file in `tests/alsa_plugin/test_<feature>.py`
2. Use pytest fixtures for setup/teardown
3. Use subprocess to call `amixer` commands
4. Verify output and exit codes

## Troubleshooting

### Mock Server Won't Start

- Check if port is already in use: `netstat -tlnp | grep 23333`
- Try a different port: `--port 23334`
- Ensure Python 3.6+ is installed: `python3 --version`

### Integration Tests Fail

- Verify mock server is running: `netstat -tlnp | grep 23333`
- Check connectivity: `telnet localhost 23333`
- Review mock server logs for errors
- Ensure DENON_TEST_HOST and DENON_TEST_PORT are set correctly

### ALSA Plugin Tests Fail

- Ensure plugin is installed: `ls -la /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_ctl_denon.so`
- Check ALSA config: `cat /etc/alsa/conf.d/50-denon-avr.conf`
- Verify permissions: user must be in `audio` group
- Check logs: `tail -f /var/log/alsa-avr/denon.log`

### Docker Tests Fail

- Ensure Docker is installed and running: `docker --version`
- Check Docker permissions: add user to `docker` group
- Rebuild images: `docker-compose build --no-cache`
- Check logs: `docker-compose logs`

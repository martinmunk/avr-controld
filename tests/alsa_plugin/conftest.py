"""
Pytest configuration and fixtures for ALSA plugin tests
"""

import pytest
import subprocess
import time
import os
import sys
from pathlib import Path

# Set ALSA config to use local test config (no system-wide install needed!)
TEST_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = TEST_DIR.parent.parent
TEST_CONFIG = TEST_DIR / "test-asound.conf"

os.environ["ALSA_CONFIG_PATH"] = str(TEST_CONFIG)

@pytest.fixture(scope="session", autouse=True)
def mock_denon_server():
    """Start mock Denon server for entire test session (if not in Docker)"""
    # Check if running in Docker (mock-denon service already running)
    in_docker = os.environ.get("DENON_HOST") == "mock-denon"

    if in_docker:
        print("\nRunning in Docker - using mock-denon service")
        # Wait for mock-denon service to be ready
        denon_host = os.environ.get("DENON_HOST", "mock-denon")
        denon_port = os.environ.get("DENON_PORT", "23")

        for _ in range(10):
            try:
                result = subprocess.run(
                    ["nc", "-z", denon_host, denon_port],
                    capture_output=True,
                    timeout=1
                )
                if result.returncode == 0:
                    print(f"Mock server {denon_host}:{denon_port} ready!")
                    break
            except:
                pass
            time.sleep(0.5)
        else:
            pytest.fail(f"Mock server {denon_host}:{denon_port} not responding")

        yield
        # No cleanup needed - Docker handles it
        return

    # Not in Docker - start local mock server
    # Check if plugin exists
    plugin_path = PROJECT_ROOT / "build" / "libasound_module_ctl_denon.so"
    if not plugin_path.exists():
        pytest.skip(f"Plugin not built: {plugin_path}")

    mock_script = PROJECT_ROOT / "tests" / "mock_server" / "denon_mock_server.py"

    # Start mock server
    print(f"\nStarting local mock Denon server on port 23333...")
    proc = subprocess.Popen(
        [sys.executable, str(mock_script), "--host", "127.0.0.1", "--port", "23333"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Wait for server to be ready
    time.sleep(1)
    for _ in range(10):
        try:
            result = subprocess.run(
                ["nc", "-z", "127.0.0.1", "23333"],
                capture_output=True,
                timeout=1
            )
            if result.returncode == 0:
                print("Mock server ready!")
                break
        except:
            pass
        time.sleep(0.5)
    else:
        proc.kill()
        pytest.fail("Mock server failed to start")

    yield

    # Cleanup local mock server
    print("\nStopping mock server...")
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except:
        proc.kill()

@pytest.fixture(scope="session")
def denon_host():
    """Get Denon host from environment"""
    return os.environ.get("DENON_HOST", "mock-denon")

@pytest.fixture(scope="session")
def denon_port():
    """Get Denon port from environment"""
    return int(os.environ.get("DENON_PORT", "23"))

@pytest.fixture(scope="session")
def alsa_control():
    """ALSA control name to use for tests"""
    # In Docker, use default card (defined in /etc/asound.conf)
    # Locally, need to specify "denon" card
    in_docker = os.environ.get("DENON_HOST") == "mock-denon"
    return "default" if in_docker else "denon"

@pytest.fixture(autouse=True)
def wait_between_tests():
    """Wait a bit between tests to let receiver settle"""
    yield
    time.sleep(0.2)

@pytest.fixture
def amixer():
    """Helper to run amixer commands"""
    def run_amixer(args, check=True, capture_output=True):
        """
        Run amixer command

        Args:
            args: List of arguments or string
            check: Raise exception on failure
            capture_output: Capture stdout/stderr

        Returns:
            CompletedProcess object
        """
        if isinstance(args, str):
            args = args.split()

        cmd = ["amixer"] + args
        result = subprocess.run(
            cmd,
            check=check,
            capture_output=capture_output,
            text=True,
            timeout=5
        )
        return result

    return run_amixer

@pytest.fixture
def get_control_value(amixer, alsa_control):
    """Helper to get current control value"""
    def _get(control_name):
        """Get integer or boolean value of a control"""
        result = amixer(["-D", alsa_control, "cget", f"name='{control_name}'"])
        # Parse output like ": values=50" or ": values=off"
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith(":") and "values=" in line:
                value_str = line.split("values=")[1].strip()
                # Handle boolean values
                if value_str == "on":
                    return 1
                elif value_str == "off":
                    return 0
                # Handle integer values
                return int(value_str)
        raise ValueError(f"Could not parse value from: {result.stdout}")

    return _get

@pytest.fixture
def set_control_value(amixer, alsa_control):
    """Helper to set control value"""
    def _set(control_name, value):
        """Set control to specific value"""
        amixer(["-D", alsa_control, "cset", f"name='{control_name}'", str(value)])

    return _set

@pytest.fixture
def restore_volume(get_control_value, set_control_value):
    """Fixture to restore volume after test"""
    original_volume = get_control_value("Master Playback Volume")
    yield
    try:
        set_control_value("Master Playback Volume", original_volume)
    except:
        pass  # Ignore restore errors

@pytest.fixture
def restore_mute(get_control_value, set_control_value):
    """Fixture to restore mute state after test"""
    original_mute = get_control_value("Master Playback Switch")
    yield
    try:
        set_control_value("Master Playback Switch", original_mute)
    except:
        pass

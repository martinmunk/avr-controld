"""
Test concurrent access to ALSA controls
"""

import pytest
import subprocess
import time
from multiprocessing import Process

@pytest.mark.usefixtures("restore_volume")
def test_multiple_simultaneous_reads(amixer, alsa_control):
    """Test multiple processes reading volume simultaneously"""
    def read_volume():
        for _ in range(10):
            result = subprocess.run(
                ["amixer", "-D", alsa_control, "cget", "name='Master Playback Volume'"],
                capture_output=True,
                timeout=5
            )
            assert result.returncode == 0
            time.sleep(0.05)

    # Start 3 processes
    processes = [Process(target=read_volume) for _ in range(3)]
    for p in processes:
        p.start()

    # Wait for all to complete
    for p in processes:
        p.join(timeout=10)
        assert p.exitcode == 0, "Process failed"

@pytest.mark.usefixtures("restore_volume")
def test_concurrent_volume_changes(set_control_value, get_control_value):
    """Test concurrent volume changes from multiple clients"""
    def change_volume(target):
        for _ in range(5):
            subprocess.run(
                ["amixer", "-D", "denon", "cset", "name='Master Playback Volume'", str(target)],
                capture_output=True,
                timeout=5
            )
            time.sleep(0.1)

    # Start 2 processes with different targets
    p1 = Process(target=change_volume, args=(40,))
    p2 = Process(target=change_volume, args=(60,))

    p1.start()
    time.sleep(0.05)  # Stagger start
    p2.start()

    p1.join(timeout=10)
    p2.join(timeout=10)

    assert p1.exitcode == 0
    assert p2.exitcode == 0

    # Final volume should be one of the targets (last writer wins)
    time.sleep(0.5)
    final = get_control_value("Master Playback Volume")
    # Should be close to either 40 or 60
    assert 35 <= final <= 65, f"Final volume {final} outside expected range"

@pytest.mark.usefixtures("restore_volume", "restore_mute")
def test_simultaneous_volume_and_mute(set_control_value, get_control_value):
    """Test changing volume and mute at the same time"""
    def change_volume():
        for val in [30, 40, 50, 60, 70]:
            subprocess.run(
                ["amixer", "-D", "denon", "cset", "name='Master Playback Volume'", str(val)],
                capture_output=True,
                timeout=5
            )
            time.sleep(0.1)

    def toggle_mute():
        for val in [1, 0, 1, 0, 1]:
            subprocess.run(
                ["amixer", "-D", "denon", "cset", "name='Master Playback Switch'", str(val)],
                capture_output=True,
                timeout=5
            )
            time.sleep(0.1)

    p1 = Process(target=change_volume)
    p2 = Process(target=toggle_mute)

    p1.start()
    p2.start()

    p1.join(timeout=10)
    p2.join(timeout=10)

    assert p1.exitcode == 0
    assert p2.exitcode == 0

    # Both controls should be in valid state
    time.sleep(0.3)
    vol = get_control_value("Master Playback Volume")
    mute = get_control_value("Master Playback Switch")

    assert 0 <= vol <= 100
    assert mute in [0, 1]

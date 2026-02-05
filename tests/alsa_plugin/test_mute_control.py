"""
Test mute control via ALSA
"""

import pytest
import time

@pytest.mark.usefixtures("restore_mute")
def test_set_mute_on(set_control_value, get_control_value):
    """Test muting"""
    set_control_value("Master Playback Switch", 1)
    time.sleep(0.3)

    value = get_control_value("Master Playback Switch")
    assert value == 1, "Mute should be on"

@pytest.mark.usefixtures("restore_mute")
def test_set_mute_off(set_control_value, get_control_value):
    """Test unmuting"""
    # First mute
    set_control_value("Master Playback Switch", 1)
    time.sleep(0.3)

    # Then unmute
    set_control_value("Master Playback Switch", 0)
    time.sleep(0.3)

    value = get_control_value("Master Playback Switch")
    assert value == 0, "Mute should be off"

@pytest.mark.usefixtures("restore_mute")
def test_mute_toggle(set_control_value, get_control_value):
    """Test toggling mute multiple times"""
    original = get_control_value("Master Playback Switch")

    # Toggle 5 times
    for i in range(5):
        expected = (original + i + 1) % 2
        set_control_value("Master Playback Switch", expected)
        time.sleep(0.2)

        actual = get_control_value("Master Playback Switch")
        assert actual == expected, f"Toggle {i}: expected {expected}, got {actual}"

@pytest.mark.usefixtures("restore_mute", "restore_volume")
def test_mute_doesnt_affect_volume(set_control_value, get_control_value):
    """Test that muting doesn't change volume level"""
    # Set specific volume
    set_control_value("Master Playback Volume", 60)
    time.sleep(0.3)
    vol_before = get_control_value("Master Playback Volume")

    # Mute
    set_control_value("Master Playback Switch", 1)
    time.sleep(0.3)

    # Check volume unchanged
    vol_during = get_control_value("Master Playback Volume")
    assert abs(vol_during - vol_before) <= 2, "Volume changed during mute"

    # Unmute
    set_control_value("Master Playback Switch", 0)
    time.sleep(0.3)

    # Check volume still unchanged
    vol_after = get_control_value("Master Playback Volume")
    assert abs(vol_after - vol_before) <= 2, "Volume changed after unmute"

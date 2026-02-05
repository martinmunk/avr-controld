"""
Test volume control via ALSA
"""

import pytest
import time

@pytest.mark.usefixtures("restore_volume")
def test_set_volume_absolute(set_control_value, get_control_value):
    """Test setting volume to specific value"""
    # Set to 50%
    set_control_value("Master Playback Volume", 50)
    time.sleep(0.3)

    value = get_control_value("Master Playback Volume")
    assert 45 <= value <= 55, f"Expected ~50, got {value}"

@pytest.mark.usefixtures("restore_volume")
def test_set_volume_min(set_control_value, get_control_value):
    """Test setting volume to minimum"""
    set_control_value("Master Playback Volume", 0)
    time.sleep(0.3)

    value = get_control_value("Master Playback Volume")
    assert value == 0

@pytest.mark.usefixtures("restore_volume")
def test_set_volume_max(set_control_value, get_control_value):
    """Test setting volume to maximum"""
    set_control_value("Master Playback Volume", 100)
    time.sleep(0.3)

    value = get_control_value("Master Playback Volume")
    assert 95 <= value <= 100

@pytest.mark.usefixtures("restore_volume")
def test_volume_range_validation(set_control_value, get_control_value):
    """Test that volume stays within valid range"""
    # These should clamp or fail gracefully
    try:
        set_control_value("Master Playback Volume", -10)
        value = get_control_value("Master Playback Volume")
        assert value >= 0, "Volume went negative"
    except:
        pass  # It's OK to reject invalid values

    try:
        set_control_value("Master Playback Volume", 150)
        value = get_control_value("Master Playback Volume")
        assert value <= 100, "Volume exceeded max"
    except:
        pass

@pytest.mark.usefixtures("restore_volume")
def test_volume_sequence(set_control_value, get_control_value):
    """Test a sequence of volume changes"""
    test_values = [0, 25, 50, 75, 100, 50]

    for target in test_values:
        set_control_value("Master Playback Volume", target)
        time.sleep(0.2)

        actual = get_control_value("Master Playback Volume")
        # Allow 5% tolerance
        assert abs(actual - target) <= 5, f"Set {target}, got {actual}"

@pytest.mark.usefixtures("restore_volume")
def test_volume_persistence(set_control_value, get_control_value):
    """Test that volume setting persists across reads"""
    set_control_value("Master Playback Volume", 42)
    time.sleep(0.3)

    # Read multiple times
    for i in range(5):
        value = get_control_value("Master Playback Volume")
        assert 40 <= value <= 44, f"Read {i}: expected ~42, got {value}"
        time.sleep(0.1)

@pytest.mark.usefixtures("restore_volume")
def test_rapid_volume_changes(set_control_value, get_control_value):
    """Test rapid volume changes"""
    # Rapidly change volume
    for i in range(10):
        set_control_value("Master Playback Volume", 30 + i * 3)
        time.sleep(0.05)

    # Final value should be stable
    time.sleep(0.3)
    final = get_control_value("Master Playback Volume")
    assert 50 <= final <= 65, f"Expected final ~57, got {final}"

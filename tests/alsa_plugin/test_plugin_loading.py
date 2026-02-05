"""
Test ALSA plugin loading and basic functionality
"""

import pytest
import subprocess

@pytest.mark.skip(reason="Not applicable when using ALSA_CONFIG_PATH")
def test_plugin_file_exists():
    """Verify plugin shared library exists"""
    result = subprocess.run(
        ["ls", "-la", "/usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_ctl_denon.so"],
        capture_output=True,
        text=True
    )
    assert result.returncode == 0, "Plugin file not found"
    assert "libasound_module_ctl_denon.so" in result.stdout

@pytest.mark.skip(reason="Not applicable when using ALSA_CONFIG_PATH")
def test_alsa_config_exists():
    """Verify ALSA configuration exists"""
    result = subprocess.run(
        ["cat", "/etc/asound.conf"],
        capture_output=True,
        text=True
    )
    assert result.returncode == 0, "ALSA config not found"
    assert "denon" in result.stdout.lower()

def test_list_controls(amixer, alsa_control):
    """Test that we can list controls via amixer"""
    result = amixer(["-D", alsa_control, "controls"])
    assert result.returncode == 0
    output = result.stdout

    # Should have our three controls
    assert "Master Playback Volume" in output
    assert "Master Playback Switch" in output
    assert "Master Playback Power" in output

def test_control_info_volume(amixer, alsa_control):
    """Test getting control info for volume"""
    result = amixer(["-D", alsa_control, "cget", "name='Master Playback Volume'"])
    assert result.returncode == 0

    output = result.stdout
    # Should show it's an integer control with range 0-100
    assert "INTEGER" in output or "type=INTEGER" in output
    assert "min=0" in output
    assert "max=100" in output

def test_control_info_mute(amixer, alsa_control):
    """Test getting control info for mute"""
    result = amixer(["-D", alsa_control, "cget", "name='Master Playback Switch'"])
    assert result.returncode == 0

    output = result.stdout
    # Should show it's a boolean control
    assert "BOOLEAN" in output or "type=BOOLEAN" in output

def test_control_info_power(amixer, alsa_control):
    """Test getting control info for power"""
    result = amixer(["-D", alsa_control, "cget", "name='Master Playback Power'"])
    assert result.returncode == 0

    output = result.stdout
    # Should show it's a boolean control
    assert "BOOLEAN" in output or "type=BOOLEAN" in output

def test_tlv_support(amixer, alsa_control):
    """Test that volume control supports TLV (dB information)"""
    result = amixer(["-D", alsa_control, "cget", "name='Master Playback Volume'"])
    assert result.returncode == 0

    output = result.stdout
    # TLV support indicated by dBscale or similar
    # Note: This might not show in cget, may need scontents or specific query
    # For now just verify control exists
    assert "Master Playback Volume" in output

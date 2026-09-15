"""Offline tests for ShairportState (pure fold of PropertiesChanged bodies)."""

from avrcontrold.shairport import IFACE_MAIN, IFACE_REMOTE, ShairportState


def test_player_state_and_playing():
    s = ShairportState()
    assert not s.playing()
    touched = s.apply_changed(IFACE_REMOTE, {"PlayerState": ("s", "Playing")})
    assert "playing" in touched and s.playing()
    # same value again -> not reported as changed
    assert s.apply_changed(IFACE_REMOTE, {"PlayerState": ("s", "Playing")}) == set()
    touched = s.apply_changed(IFACE_REMOTE, {"PlayerState": ("s", "Paused")})
    assert "playing" in touched and not s.playing()


def test_active():
    s = ShairportState()
    touched = s.apply_changed(IFACE_MAIN, {"Active": ("b", True)})
    assert "active" in touched and s.active is True


def test_volume():
    s = ShairportState()
    touched = s.apply_changed(IFACE_REMOTE, {"AirplayVolume": ("d", -22.5)})
    assert "volume" in touched and s.volume_db == -22.5


def test_metadata_variant_forms():
    s = ShairportState()
    meta = ("a{sv}", {
        "xesam:title": ("s", "Song"),
        "xesam:album": ("s", "Album"),
        "xesam:artist": ("as", ["Artist One", "Artist Two"]),
    })
    touched = s.apply_changed(IFACE_REMOTE, {"Metadata": meta})
    assert "metadata" in touched
    assert s.title == "Song"
    assert s.album == "Album"
    assert s.artist == "Artist One"     # first of the array


def test_snapshot_format_and_sanitize():
    s = ShairportState()
    s.apply_changed(IFACE_REMOTE, {
        "PlayerState": ("s", "Playing"),
        "Metadata": ("a{sv}", {"xesam:title": ("s", "a|b\nc"), "xesam:artist": ("as", ["X"])}),
    })
    s.apply_changed(IFACE_MAIN, {"Active": ("b", True)})
    snap = s.snapshot()
    assert "state=Playing" in snap and "active=1" in snap
    assert "title=a b c" in snap        # | and newline sanitized
    assert "artist=X" in snap


def test_unrelated_interface_ignored():
    s = ShairportState()
    assert s.apply_changed("org.freedesktop.DBus.Properties", {"Whatever": ("s", "x")}) == set()


def test_plain_values_without_variant_wrapper():
    # tolerate already-unwrapped values
    s = ShairportState()
    s.apply_changed(IFACE_REMOTE, {"PlayerState": "Playing"})
    assert s.playing()

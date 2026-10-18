"""Offline unit tests for the safety-critical volume clamp (no hardware)."""

from avrcontrold.volume import (
    VOLUME_MAX,
    clamp_absolute,
    clamp_ceiling,
    allow_step_up,
)


def test_clamp_ceiling_bounds():
    assert clamp_ceiling(-5) == 0
    assert clamp_ceiling(0) == 0
    assert clamp_ceiling(40) == 40
    assert clamp_ceiling(100) == 100
    assert clamp_ceiling(150) == 100


def test_clamp_absolute_within_range():
    assert clamp_absolute(30, 80) == 30
    assert clamp_absolute(0, 80) == 0
    assert clamp_absolute(80, 80) == 80


def test_clamp_absolute_enforces_ceiling():
    # The blast scenario: a stray "set 100" must never reach the receiver.
    assert clamp_absolute(100, 40) == 40
    assert clamp_absolute(999, 40) == 40


def test_clamp_absolute_floor():
    assert clamp_absolute(-1, 80) == 0
    assert clamp_absolute(-100, 80) == 0


def test_clamp_absolute_no_cap_means_100():
    assert clamp_absolute(100, VOLUME_MAX) == 100
    assert clamp_absolute(70, VOLUME_MAX) == 70


def test_clamp_absolute_bad_ceiling_is_normalised():
    assert clamp_absolute(50, -10) == 0     # ceiling below floor -> nothing allowed
    assert clamp_absolute(50, 500) == 50    # ceiling above 100 -> treated as 100


def test_allow_step_up_unknown_current():
    assert allow_step_up(None, 40) is True


def test_allow_step_up_below_ceiling():
    assert allow_step_up(39, 40) is True


def test_allow_step_up_at_or_above_ceiling():
    assert allow_step_up(40, 40) is False
    assert allow_step_up(55, 40) is False


# ---- AirPlay dB mapping ----------------------------------------------------
from avrcontrold.volume import airplay_db_to_pct, is_airplay_mute, AIRPLAY_MUTE_DB


def test_airplay_db_to_pct_endpoints():
    assert airplay_db_to_pct(0.0) == 100
    assert airplay_db_to_pct(-30.0) == 0
    assert airplay_db_to_pct(-15.0) == 50          # midpoint of [-30,0]


def test_airplay_db_to_pct_clamps_out_of_range():
    assert airplay_db_to_pct(5.0) == 100
    assert airplay_db_to_pct(-100.0) == 0


def test_airplay_db_to_pct_custom_range():
    assert airplay_db_to_pct(-20.0, min_db=-40.0, max_db=0.0) == 50


def test_is_airplay_mute():
    assert is_airplay_mute(AIRPLAY_MUTE_DB)
    assert is_airplay_mute(-144.0)
    assert not is_airplay_mute(-30.0)
    assert not is_airplay_mute(0.0)


# ---- "standard" curve (shairport-sync vol2attn) -----------------------------
from avrcontrold.volume import airplay_standard_fraction, AIRPLAY_CURVES
import pytest


def _vol2attn(vol, max_db, min_db):
    """Literal port of shairport-sync common.c vol2attn (standard profile),
    used as the oracle: the daemon's curve must reproduce it for any range."""
    range_db = max_db - min_db
    first_slope = -range_db / 2
    if -range_db > first_slope:
        first_slope = range_db
    lines = [[0, first_slope], [-5, first_slope - (range_db + first_slope) / 2],
             [-17, -range_db]]
    vol_setting = 0.0
    for x0, y_end in lines:
        if vol <= x0:
            tvol = y_end * (vol - x0) / (-30 - x0)
            if tvol < vol_setting:
                vol_setting = tvol
    return vol_setting + max_db


@pytest.mark.parametrize("min_db,max_db", [(-80, 0), (-99, 0), (-30, 0), (-60, -10)])
def test_standard_curve_matches_shairport_for_any_range(min_db, max_db):
    rng = max_db - min_db
    for tenth in range(-300, 1):
        vol = tenth / 10
        oracle = (_vol2attn(vol, max_db, min_db) - min_db) / rng
        # compare the fractions (rounding a float that lands exactly on .5 is
        # noise, not a curve difference)
        assert airplay_standard_fraction(vol) == pytest.approx(oracle, abs=1e-9), vol


def test_standard_curve_landmarks():
    # endpoints are the endpoints, and the taper lifts the lower slider range
    assert airplay_db_to_pct(-30.0, curve="standard") == 0
    assert airplay_db_to_pct(0.0, curve="standard") == 100
    assert airplay_db_to_pct(-20.0, curve="standard") == 55   # a third of the slider
    assert airplay_db_to_pct(-15.0, curve="standard") == 70   # half
    assert airplay_db_to_pct(-20.0, curve="flat") == 33
    assert airplay_standard_fraction(-15.0) == pytest.approx(0.7)


def test_standard_curve_is_monotonic():
    last = -1
    for tenth in range(-300, 1):
        pct = airplay_db_to_pct(tenth / 10, curve="standard")
        assert pct >= last
        last = pct


def test_standard_curve_respects_custom_min_db():
    # the taper is applied over [min_db, max_db], so the bottom still maps to 0
    assert airplay_db_to_pct(-40.0, min_db=-40.0, curve="standard") == 0
    assert airplay_db_to_pct(-20.0, min_db=-40.0, curve="standard") == 70   # half


def test_unknown_curve_rejected():
    assert AIRPLAY_CURVES == ("flat", "standard")
    with pytest.raises(ValueError):
        airplay_db_to_pct(-10.0, curve="log")

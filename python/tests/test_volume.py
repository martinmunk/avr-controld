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

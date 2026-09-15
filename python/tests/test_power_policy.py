"""Offline tests for the generic, backend-agnostic PowerPolicy."""

from avrcontrold.power_policy import PowerPolicy


def test_demand_powers_on():
    p = PowerPolicy(off_grace_s=120)
    assert p.set_demand("airplay", True, now_s=0) is True   # power on
    assert p.set_demand("airplay", True, now_s=1) is None    # no change


def test_clearing_last_demand_schedules_off_not_immediate():
    p = PowerPolicy(off_grace_s=120)
    p.set_demand("airplay", True, 0)
    assert p.set_demand("airplay", False, 10) is None        # only scheduled
    assert p.next_deadline() == 130
    assert p.tick(129) is None                               # not yet
    assert p.tick(130) is False                              # now power off
    assert p.tick(131) is None                               # already off


def test_new_demand_cancels_pending_off():
    p = PowerPolicy(off_grace_s=120)
    p.set_demand("airplay", True, 0)
    p.set_demand("airplay", False, 10)                       # off scheduled @130
    assert p.set_demand("tv", True, 60) is None              # still on, cancel off
    assert p.next_deadline() is None
    assert p.tick(200) is None                               # stays on


def test_multiple_demands_stay_on_until_all_clear():
    p = PowerPolicy(off_grace_s=60)
    assert p.set_demand("airplay", True, 0) is True
    assert p.set_demand("tv", True, 5) is None               # already on
    assert p.set_demand("airplay", False, 10) is None        # tv still active -> on
    assert p.next_deadline() is None                          # no off scheduled
    assert p.set_demand("tv", False, 20) is None             # now schedule off
    assert p.next_deadline() == 80
    assert p.tick(80) is False


def test_no_demand_at_startup_does_not_force_off():
    p = PowerPolicy()
    # Never had a demand; must not command power off (don't fight a hand-on).
    assert p.set_demand("airplay", False, 0) is None
    assert p.next_deadline() is None
    assert p.tick(10_000) is None


def test_reassert_after_off_powers_on_again():
    p = PowerPolicy(off_grace_s=30)
    p.set_demand("airplay", True, 0)
    p.set_demand("airplay", False, 0)
    assert p.tick(30) is False                               # off
    assert p.set_demand("airplay", True, 40) is True         # on again


def test_toggle_within_grace_no_spurious_commands():
    p = PowerPolicy(off_grace_s=120)
    assert p.set_demand("airplay", True, 0) is True
    assert p.set_demand("airplay", False, 10) is None        # schedule off
    assert p.set_demand("airplay", True, 20) is None         # still on, cancel off
    assert p.tick(200) is None                               # no off happened

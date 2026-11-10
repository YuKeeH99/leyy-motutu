#!/usr/bin/env python3
"""Contracts for the 2x flip timeline while preserving expressive beats."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GAIT_C = (ROOT / "apps/arcs-mini/services/service_v2_gait.c").read_text(encoding="utf-8")
GAIT_H = (ROOT / "apps/arcs-mini/services/service_v2_gait.h").read_text(encoding="utf-8")


def macro(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)U\s*$", source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing numeric macro: {name}")
    return int(match.group(1))


class V2FlipSpeedContract(unittest.TestCase):
    def test_main_flip_stages_are_twice_as_fast(self) -> None:
        expected = {
            "GAIT_FLIP_HOME_MOVE_MS": 250,
            "GAIT_FLIP_FRONT_START_MS": 350,
            "GAIT_FLIP_FRONT_MOVE_MS": 125,
            "GAIT_FLIP_REAR_START_MS": 675,
            "GAIT_FLIP_REAR_MOVE_MS": 425,
            "GAIT_FLIP_HOOK_START_MS": 1330,
            "GAIT_FLIP_HOOK_MOVE_MS": 1100,
            "GAIT_ROLL_REVERSE_FRONT_RETRACT_MS": 350,
            "GAIT_ROLL_REVERSE_REAR_STAGE_MS": 300,
            "GAIT_ROLL_REVERSE_HOOK_MS": 1100,
            "GAIT_ROLL_REAR_RETURN_MS": 350,
            "GAIT_ROLL_CROSS_MS": 225,
            "GAIT_ROLL_RECOVER_PAIR_MS": 250,
            "GAIT_ROLL_CYCLE_HOLD_MS": 750,
        }
        for name, value in expected.items():
            self.assertEqual(macro(GAIT_C, name), value, name)
        self.assertEqual(macro(GAIT_H, "SERVICE_V2_GAIT_FLIP_RISE_DURATION_MS"), 2430)

    def test_expressive_and_safety_timing_is_unchanged(self) -> None:
        expected = {
            "GAIT_ROLL_DANCE_ENTRY_MS": 600,
            "GAIT_ROLL_DANCE_HALF_BEAT_MS": 160,
            "GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS": 350,
            "GAIT_ROLL_LANDING_TIMEOUT_MS": 2500,
            "GAIT_ROLL_LANDING_SAMPLE_MS": 20,
            "GAIT_ROLL_START_HOLD_MS": 800,
        }
        for name, value in expected.items():
            self.assertEqual(macro(GAIT_C, name), value, name)


if __name__ == "__main__":
    unittest.main()

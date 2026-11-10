#!/usr/bin/env python3
"""Source-level regression tests for the field-verified V2 gait actions.

The gait player is tightly coupled to FreeRTOS and the actuator service, so these
host-only tests lock the exact keyframes and timing contract without trying to
execute target firmware.  In particular, they prevent the PA13 kick and tennis
serve from regressing to a single, simultaneous sweep from the seated strike
pose to HOME.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
GAIT_SOURCE_PATH = REPO_ROOT / "apps/arcs-mini/services/service_v2_gait.c"
GAIT_HEADER_PATH = REPO_ROOT / "apps/arcs-mini/services/service_v2_gait.h"
VOICE_ACTION_PATH = REPO_ROOT / "apps/arcs-mini/services/service_voice_action.c"

SMOOTH = "SERVICE_V2_SERVO_EASING_SMOOTH"
LINEAR = "SERVICE_V2_SERVO_EASING_LINEAR"
SETTLED_HOLD_MS = 100

HOME = (2100, 900, 2050, 870)
ASSEMBLY = (1500, 1500, 1615, 1315)
REAR_PRELOAD_STAGE1 = (2100, 900, 2150, 800)
REAR_PRELOAD = (2100, 900, 2200, 750)
REAR_HOME_MS = 500
APPROACH = (2100, 1400, 2200, 750)
STRIKE_BASE = (2100, 1800, 2200, 750)
STRIKE_FORWARD = (2100, 2000, 2200, 750)
TENNIS_REAR_KICK = (2100, 900, 1615, 1315)
TENNIS_REAR_PRELOAD_STAGE1 = (2100, 900, 2180, 770)
TENNIS_REAR_PRELOAD = (2100, 900, 2230, 720)
LEFT_HANDSHAKE_BASE = (1600, 900, 2200, 750)
LEFT_HANDSHAKE_WAVE = (1400, 900, 2200, 750)
RIGHT_HANDSHAKE_BASE = (2100, 1400, 2200, 750)
RIGHT_HANDSHAKE_WAVE = (2100, 1600, 2200, 750)
SIT = (2100, 900, 2300, 700)
GREETING_BASE = (2100, 1500, 2200, 750)
GREETING_WAVE = (2100, 1800, 2200, 750)
SCRATCH_BASE = (2100, 2000, 2200, 750)
SCRATCH_WAVE = (2100, 2300, 2200, 750)
JUMP_KICK = ASSEMBLY
BUTT_WIGGLE_LEFT = (2100, 900, 2180, 1000)
BUTT_WIGGLE_RIGHT = (2100, 900, 1920, 740)
DANCE_SQUAT = (2070, 930, 2110, 810)
DANCE_POP = (2100, 900, 1990, 930)
DANCE_DIAGONAL_A = (2040, 840, 2110, 930)
DANCE_DIAGONAL_B = (2160, 960, 1990, 810)
DANCE_LEFT = (1500, 900, 2110, 930)
DANCE_RIGHT = (2100, 1500, 1990, 810)
DANCE_HIP_LEFT = (2100, 900, 2100, 920)
DANCE_HIP_RIGHT = (2100, 900, 2000, 820)
DANCE_RECOVER_SUPPORT = (2100, 900, 1990, 930)
REVERSE_HOME = (1100, 1900, 1150, 1900)
UPSIDE_FEEDBACK_A = (2040, 840, 2110, 930)
UPSIDE_FEEDBACK_B = (2160, 960, 1990, 810)
ROLL_REVERSE_WIGGLE_A = (1040, 1840, 1210, 1960)
ROLL_REVERSE_WIGGLE_B = (1160, 1960, 1090, 1840)
STANDBY_REAR_FOLDED = (2100, 900, 2500, 500)
STANDBY_ALL_FOLDED = (500, 2500, 2500, 500)


def function_body(source: str, name: str, return_type: str = "void") -> str:
    """Return a C function body using balanced braces."""

    match = re.search(
        rf"\bstatic\s+{re.escape(return_type)}\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing static {return_type} function: {name}")

    opening_brace = source.find("{", match.start())
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace + 1 : index]
    raise AssertionError(f"unterminated function body: {name}")


class GaitSource:
    _DEFINE_RE = re.compile(r"^#define\s+([A-Z][A-Z0-9_]*)\s+(\d+)U?\s*$", re.MULTILINE)
    _FRAME_RE = re.compile(
        r"static\s+const\s+uint16_t\s+([a-zA-Z_][a-zA-Z0-9_]*)"
        r"\s*\[[^]]+\]\s*=\s*\{(.*?)\}\s*;",
        re.DOTALL,
    )
    _SETTLED_CALL_RE = re.compile(
        r"play_frame_settled\s*\(\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*,\s*"
        r"([A-Z][A-Z0-9_]*|\d+U?)\s*,\s*"
        r"([A-Z][A-Z0-9_]*)\s*\)",
        re.DOTALL,
    )
    _HOLD_ASSIGN_RE = re.compile(
        r"\bret\s*=\s*hold_pose\s*\(\s*([A-Z][A-Z0-9_]*|\d+U?)\s*\)",
        re.DOTALL,
    )

    def __init__(self) -> None:
        self.source = GAIT_SOURCE_PATH.read_text(encoding="utf-8")
        self.header = GAIT_HEADER_PATH.read_text(encoding="utf-8")
        self.defines = {
            name: int(value)
            for name, value in self._DEFINE_RE.findall(self.source + "\n" + self.header)
        }
        self.frame_tokens = {
            name: tuple(part.strip() for part in initializer.split(",") if part.strip())
            for name, initializer in self._FRAME_RE.findall(self.source)
        }

    def value(self, token: str) -> int:
        literal = token.removesuffix("U")
        if literal.isdigit():
            return int(literal)
        if token not in self.defines:
            raise AssertionError(f"cannot resolve integer token: {token}")
        return self.defines[token]

    def frame(self, name: str) -> tuple[int, ...]:
        if name not in self.frame_tokens:
            raise AssertionError(f"missing gait frame: {name}")
        return tuple(self.value(token) for token in self.frame_tokens[name])

    def settled_calls(self, body: str) -> list[tuple[tuple[int, ...], int, str]]:
        return [
            (self.frame(frame_name), self.value(duration), easing)
            for frame_name, duration, easing in self._SETTLED_CALL_RE.findall(body)
        ]

    def assigned_hold_calls(self, body: str) -> list[int]:
        return [self.value(duration) for duration in self._HOLD_ASSIGN_RE.findall(body)]

    def recovery_helper(self) -> tuple[str, str]:
        """Find the one helper that implements the verified two-beat recovery."""

        expected = [
            (REAR_PRELOAD, 500, SMOOTH),
            (HOME, REAR_HOME_MS, SMOOTH),
        ]
        candidates: list[tuple[str, str]] = []
        for name in re.findall(r"\bstatic\s+int\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\(", self.source):
            body = function_body(self.source, name, "int")
            if self.settled_calls(body) == expected:
                candidates.append((name, body))

        if len(candidates) != 1:
            names = [name for name, _ in candidates]
            raise AssertionError(
                "expected exactly one shared 500/500 ms two-beat HOME helper, "
                f"found {names}"
            )
        return candidates[0]


class ServiceV2GaitContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gait = GaitSource()

    def test_field_verified_frames_and_cycle_counts_are_exact(self) -> None:
        self.assertEqual(HOME, self.gait.frame("s_pose_stand"))
        self.assertEqual(ASSEMBLY, self.gait.frame("s_pose_assembly"))
        self.assertEqual(REAR_PRELOAD_STAGE1, self.gait.frame("s_right_front_kick_rear_preload_stage1"))
        self.assertEqual(REAR_PRELOAD, self.gait.frame("s_right_front_kick_rear_preload"))
        self.assertEqual(APPROACH, self.gait.frame("s_right_front_kick_approach"))
        self.assertEqual(STRIKE_BASE, self.gait.frame("s_right_front_kick_base"))
        self.assertEqual(STRIKE_FORWARD, self.gait.frame("s_right_front_kick_forward"))
        self.assertEqual(6, self.gait.defines["SERVICE_V2_GAIT_RIGHT_FRONT_KICK_CYCLES"])
        self.assertEqual(1, self.gait.defines["SERVICE_V2_GAIT_TENNIS_SERVE_CYCLES"])

    def test_assembly_pose_moves_smoothly_and_keeps_pwm_enabled(self) -> None:
        body = function_body(self.gait.source, "run_pose")

        self.assertEqual(700, self.gait.defines["GAIT_ASSEMBLY_MOVE_MS"])
        self.assertRegex(
            body,
            r"action\s*==\s*SERVICE_V2_GAIT_ACTION_ASSEMBLY[\s\S]*?pose\s*=\s*s_pose_assembly",
        )
        self.assertRegex(body, r"duration_ms\s*=\s*GAIT_ASSEMBLY_MOVE_MS")
        self.assertRegex(
            body,
            r"if\s*\(\s*ret\s*==\s*0\s*&&\s*action\s*==\s*SERVICE_V2_GAIT_ACTION_PRONE\s*\)"
            r"[\s\S]*?servo_disable_all",
            "only prone should turn PWM off after reaching its target",
        )

    def test_standby_fold_stands_then_folds_rear_then_front(self) -> None:
        self.assertEqual(
            STANDBY_REAR_FOLDED,
            self.gait.frame("s_standby_rear_folded"),
        )
        self.assertEqual(
            STANDBY_ALL_FOLDED,
            self.gait.frame("s_standby_all_folded"),
        )

        body = function_body(self.gait.source, "run_standby_fold")
        self.assertEqual(
            [
                (HOME, 500, SMOOTH),
                (STANDBY_REAR_FOLDED, 1200, SMOOTH),
                (STANDBY_ALL_FOLDED, 1200, SMOOTH),
            ],
            self.gait.settled_calls(body),
            "standby must stand first, fold both rear legs, then rotate both front legs forward",
        )

    def test_usb_only_blocks_rolls_not_normal_face_actions(self) -> None:
        normal = function_body(self.gait.source, "prepare_normal_face_motion", "int")
        roll = function_body(self.gait.source, "roll_power_check", "int")
        pending = function_body(self.gait.source, "cmd_pending", "bool")
        upside = function_body(self.gait.source, "run_upside_wiggle")
        self.assertNotIn("roll_power_check", normal)
        self.assertNotIn("power_is_usb_plugged", normal)
        self.assertIn("power_is_usb_plugged", roll)
        self.assertNotIn("action_is_normal_face_motion", pending)
        self.assertNotIn("s_normal_face_motion_reserved", pending)
        self.assertNotIn("roll_power_check", upside)

    def test_manual_upside_feedback_keeps_paws_up_in_normal_joint_pose(self) -> None:
        body = function_body(self.gait.source, "run_upside_wiggle")

        self.assertIn("sync_roll_home_from_disabled(ROLL_FACE_NORMAL)", body)
        self.assertIn("s_upside_feedback_wiggle_a", body)
        self.assertIn("s_upside_feedback_wiggle_b", body)
        self.assertIn("play_frame(s_pose_stand", body)
        self.assertNotIn("sync_roll_home_from_disabled(ROLL_FACE_REVERSE)", body)
        self.assertNotIn("play_frame(s_pose_reverse_home", body)

    def test_shared_recovery_returns_front_then_rear_with_settled_smooth_frames(self) -> None:
        helper_name, helper_body = self.gait.recovery_helper()
        calls = list(self.gait._SETTLED_CALL_RE.finditer(helper_body))
        holds = list(self.gait._HOLD_ASSIGN_RE.finditer(helper_body))

        self.assertEqual(2, len(calls))
        self.assertEqual(
            [SETTLED_HOLD_MS],
            self.gait.assigned_hold_calls(helper_body),
            "the shared recovery must add exactly one extra 100 ms hold after final HOME; "
            "the first beat already receives the common settled-frame hold",
        )
        self.assertLess(calls[1].end(), holds[0].start())
        self.assertRegex(
            helper_body[calls[0].end() : calls[1].start()],
            r"\bif\s*\(\s*ret\s*==\s*0\s*\)",
            "the rear-HOME beat must not run unless the front beat, including its common hold, succeeded",
        )
        self.assertRegex(
            helper_body[calls[1].end() : holds[0].start()],
            r"\bif\s*\(\s*ret\s*==\s*0\s*\)",
            "the final extra hold must not run unless the rear-HOME beat settled successfully",
        )
        self.assertRegex(helper_body, r"\breturn\s+ret\s*;")
        self.assertNotEqual("", helper_name)

    def test_every_settled_frame_holds_100ms_after_reaching_its_target(self) -> None:
        body = function_body(self.gait.source, "play_frame_settled", "int")
        play = re.search(r"\b(?:int\s+)?ret\s*=\s*play_frame\s*\(", body)
        wait = re.search(r"\bret\s*=\s*wait_frame_target\s*\(\s*pulse_us\s*\)", body)
        holds = list(self.gait._HOLD_ASSIGN_RE.finditer(body))

        self.assertIsNotNone(play, "play_frame_settled must first submit and await the PWM frame")
        self.assertRegex(
            body,
            r"\bplay_frame\s*\(\s*pulse_us\s*,\s*duration_ms\s*,\s*easing\s*\)",
            "play_frame_settled must pass the caller's frame, duration and easing through unchanged",
        )
        self.assertIsNotNone(wait, "play_frame_settled must preserve the explicit target-settle check")
        self.assertEqual(
            [SETTLED_HOLD_MS],
            self.gait.assigned_hold_calls(body),
            "every successfully settled frame must hold its target for exactly 100 ms",
        )
        assert play is not None
        assert wait is not None
        self.assertLess(play.end(), wait.start())
        self.assertLess(wait.end(), holds[0].start())
        self.assertRegex(
            body[play.end() : wait.start()],
            r"\bif\s*\(\s*ret\s*==\s*0\s*\)",
            "the target-settle check must only run after play_frame succeeds",
        )
        self.assertRegex(
            body[wait.end() : holds[0].start()],
            r"\bif\s*\(\s*ret\s*==\s*0\s*\)",
            "the 100 ms hold must only run after wait_frame_target succeeds",
        )
        self.assertRegex(body[holds[0].end() :], r"\breturn\s+ret\s*;")

    def test_pa13_kick_runs_six_strikes_then_calls_shared_recovery(self) -> None:
        helper_name, _ = self.gait.recovery_helper()
        body = function_body(self.gait.source, "run_right_front_kick")
        prepare = body.find('prepare_normal_face_motion("right-front kick"')
        settled = list(self.gait._SETTLED_CALL_RE.finditer(body))

        self.assertEqual(
            [
                (REAR_PRELOAD_STAGE1, 700, SMOOTH),
                (REAR_PRELOAD, 400, SMOOTH),
                (APPROACH, 500, SMOOTH),
                (STRIKE_BASE, 260, SMOOTH),
                (STRIKE_FORWARD, 120, LINEAR),
                (STRIKE_BASE, 120, LINEAR),
            ],
            self.gait.settled_calls(body),
        )
        self.assertGreaterEqual(prepare, 0)
        self.assertLess(prepare, settled[0].start())
        for previous, current in zip(settled, settled[1:]):
            self.assertRegex(
                body[previous.end() : current.start()],
                r"\bret\s*==\s*0\b",
                "every kick frame must be gated by the previous frame succeeding",
            )
        self.assertRegex(
            body,
            r"cycle\s*<\s*SERVICE_V2_GAIT_RIGHT_FRONT_KICK_CYCLES",
        )
        helper_calls = list(re.finditer(rf"\b{re.escape(helper_name)}\s*\(", body))
        self.assertEqual(1, len(helper_calls))
        self.assertGreater(helper_calls[0].start(), body.rfind("play_frame_settled"))

    def test_tennis_serve_runs_one_strike_then_uses_fast_front_recovery(self) -> None:
        helper_name, _ = self.gait.recovery_helper()
        body = function_body(self.gait.source, "run_tennis_serve")
        prepare = body.find('prepare_normal_face_motion("tennis serve"')
        settled = list(self.gait._SETTLED_CALL_RE.finditer(body))

        self.assertEqual(
            [
                (TENNIS_REAR_PRELOAD_STAGE1, 700, SMOOTH),
                (TENNIS_REAR_PRELOAD, 400, SMOOTH),
                ((2100, 1400, 2230, 720), 500, SMOOTH),
                ((2100, 1800, 2230, 720), 260, SMOOTH),
                ((2100, 2000, 2230, 720), 120, LINEAR),
                ((2100, 1800, 2230, 720), 60, LINEAR),
            ],
            self.gait.settled_calls(body),
        )
        self.assertGreaterEqual(prepare, 0)
        self.assertLess(prepare, settled[0].start())
        for previous, current in zip(settled, settled[1:]):
            self.assertRegex(
                body[previous.end() : current.start()],
                r"\bret\s*==\s*0\b",
                "every serve frame must be gated by the previous frame succeeding",
            )
        self.assertRegex(
            body,
            r"done\s*=\s*SERVICE_V2_GAIT_TENNIS_SERVE_CYCLES\s*;",
        )
        self.assertNotRegex(body, rf"\b{re.escape(helper_name)}\s*\(")
        self.assertRegex(body, r"\brecover_tennis_serve_home\s*\(")
        recovery = function_body(self.gait.source, "recover_tennis_serve_home", "int")
        self.assertEqual(
            [
                (TENNIS_REAR_PRELOAD, 20, LINEAR),
                (TENNIS_REAR_KICK, 60, LINEAR),
                (HOME, 120, SMOOTH),
            ],
            self.gait.settled_calls(recovery),
        )

    def test_extended_product_actions_are_declared_and_dispatched(self) -> None:
        required = (
            "LEFT_FRONT_HANDSHAKE",
            "RIGHT_FRONT_HANDSHAKE",
            "GREETING",
            "SCRATCH",
            "SPIN",
            "SIT",
            "KISS",
            "DANCE",
            "JUMP",
            "WIGGLE_BUTT",
            "UPSIDE_WIGGLE",
            "TENNIS_FOREHAND",
            "TENNIS_BACKHAND",
            "TENNIS_DOUBLE_BACKHAND",
        )
        for suffix in required:
            token = f"SERVICE_V2_GAIT_ACTION_{suffix}"
            self.assertIn(token, self.gait.header)
            self.assertIn(token, self.gait.source)

    def test_groundstrokes_have_run_turn_jump_strike_and_recovery_stages(self) -> None:
        body = function_body(self.gait.source, "run_tennis_groundstroke")
        self.assertIn("play_tennis_run_approach", body)
        self.assertIn("play_tennis_turn_approach", body)
        self.assertIn("s_pose_sit", body)
        for frame in (
            "s_tennis_forehand_ready",
            "s_tennis_forehand_strike",
            "s_tennis_backhand_ready",
            "s_tennis_backhand_strike",
            "s_tennis_double_backhand_ready",
            "s_tennis_double_backhand_strike",
        ):
            self.assertIn(frame, body)
            values = self.gait.frame(frame)
            self.assertTrue(all(700 <= value <= 2300 for value in values), frame)
        self.assertIn("recover_normal_face_home_two_stage", body)

    def test_groundstrokes_choose_a_random_sharp_turn_after_the_run(self) -> None:
        body = function_body(self.gait.source, "run_tennis_groundstroke")
        run_at = body.find("play_tennis_run_approach()")
        choose_at = body.find("choose_tennis_turn_left()")
        turn_at = body.find("play_tennis_turn_approach(turn_left)")
        strike_at = body.find("play_frame_settled(strike")

        self.assertTrue(0 <= run_at < choose_at < turn_at < strike_at)
        self.assertEqual(3, self.gait.defines["GAIT_TENNIS_SHARP_TURN_CYCLES"])
        self.assertEqual(60, self.gait.defines["GAIT_TENNIS_SHARP_TURN_FRAME_MS"])
        turn_body = function_body(self.gait.source, "play_tennis_turn_approach", "int")
        self.assertIn("GAIT_TENNIS_SHARP_TURN_CYCLES", turn_body)
        self.assertIn("GAIT_TENNIS_SHARP_TURN_FRAME_MS", turn_body)
        self.assertIn('turn_left ? "left" : "right"', body)

    def test_tennis_downstroke_is_faster_than_ordinary_kick_beat(self) -> None:
        self.assertIn("GAIT_TENNIS_STRIKE_MS", self.gait.defines)
        self.assertLess(
            self.gait.defines["GAIT_TENNIS_STRIKE_MS"],
            self.gait.defines["GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS"],
        )

    def test_product_pose_frames_match_the_requested_mechanics(self) -> None:
        self.assertEqual(LEFT_HANDSHAKE_BASE, self.gait.frame("s_left_front_handshake_base"))
        self.assertEqual(LEFT_HANDSHAKE_WAVE, self.gait.frame("s_left_front_handshake_wave"))
        self.assertEqual(RIGHT_HANDSHAKE_BASE, self.gait.frame("s_right_front_handshake_base"))
        self.assertEqual(RIGHT_HANDSHAKE_WAVE, self.gait.frame("s_right_front_handshake_wave"))
        self.assertEqual(SIT, self.gait.frame("s_pose_sit"))
        self.assertEqual(GREETING_BASE, self.gait.frame("s_greeting_base"))
        self.assertEqual(GREETING_WAVE, self.gait.frame("s_greeting_wave"))
        self.assertEqual(SCRATCH_BASE, self.gait.frame("s_scratch_base"))
        self.assertEqual(SCRATCH_WAVE, self.gait.frame("s_scratch_wave"))
        self.assertEqual(JUMP_KICK, self.gait.frame("s_jump_kick"))

    def test_handshake_stays_near_the_horizontal_assembly_angle(self) -> None:
        for frame in (
            LEFT_HANDSHAKE_BASE,
            LEFT_HANDSHAKE_WAVE,
            RIGHT_HANDSHAKE_BASE,
            RIGHT_HANDSHAKE_WAVE,
        ):
            raised_front_us = frame[0] if frame[0] != HOME[0] else frame[1]
            self.assertLessEqual(abs(raised_front_us - 1500), 100)

        body = function_body(self.gait.source, "run_paw_wave")
        self.assertIn("s_left_front_handshake_base", body)
        self.assertIn("s_left_front_handshake_wave", body)
        self.assertIn("s_right_front_handshake_base", body)
        self.assertIn("s_right_front_handshake_wave", body)

    def test_jump_sits_kicks_all_legs_out_then_returns_home_fast(self) -> None:
        body = function_body(self.gait.source, "run_product_trick")
        jump = re.search(
            r"action\s*==\s*SERVICE_V2_GAIT_ACTION_JUMP\s*\)([\s\S]*?)\n\s*}\n",
            body,
        )
        self.assertIsNotNone(jump, "missing DOG_JUMP branch")
        branch = jump.group(1)
        calls = self.gait.settled_calls(branch)
        self.assertEqual(
            [
                (SIT, 500, SMOOTH),
                (JUMP_KICK, 80, LINEAR),
                (HOME, 80, LINEAR),
            ],
            calls,
        )

    def test_wiggle_butt_keeps_front_legs_home_and_alternates_rear_legs(self) -> None:
        self.assertEqual(BUTT_WIGGLE_LEFT, self.gait.frame("s_butt_wiggle_left"))
        self.assertEqual(BUTT_WIGGLE_RIGHT, self.gait.frame("s_butt_wiggle_right"))
        self.assertEqual(BUTT_WIGGLE_LEFT[:2], HOME[:2])
        self.assertEqual(BUTT_WIGGLE_RIGHT[:2], HOME[:2])

        body = function_body(self.gait.source, "run_product_trick")
        self.assertRegex(
            body,
            r"SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT[\s\S]*?"
            r"SERVICE_V2_GAIT_WIGGLE_BUTT_CYCLES[\s\S]*?"
            r"s_butt_wiggle_left[\s\S]*?s_butt_wiggle_right[\s\S]*?"
            r"recover_normal_face_home_two_stage",
        )

        voice_source = VOICE_ACTION_PATH.read_text(encoding="utf-8")
        self.assertIn(
            '{"DOG_WIGGLE_BUTT", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT}',
            voice_source,
        )
        self.assertIn(
            '{"WIGGLE_BUTT", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT}',
            voice_source,
        )

    def test_dance_uses_a_safe_12_second_robot_sequence_and_forced_stand_recovery(self) -> None:
        self.assertEqual(DANCE_SQUAT, self.gait.frame("s_dance_squat"))
        self.assertEqual(DANCE_POP, self.gait.frame("s_dance_pop"))
        self.assertEqual(DANCE_DIAGONAL_A, self.gait.frame("s_dance_diagonal_a"))
        self.assertEqual(DANCE_DIAGONAL_B, self.gait.frame("s_dance_diagonal_b"))
        self.assertEqual(DANCE_LEFT, self.gait.frame("s_dance_left_paw"))
        self.assertEqual(DANCE_RIGHT, self.gait.frame("s_dance_right_paw"))
        self.assertEqual(DANCE_HIP_LEFT, self.gait.frame("s_dance_hip_left"))
        self.assertEqual(DANCE_HIP_RIGHT, self.gait.frame("s_dance_hip_right"))
        self.assertEqual(DANCE_RECOVER_SUPPORT, self.gait.frame("s_dance_recover_support"))
        self.assertEqual(250, self.gait.defines["GAIT_DANCE_HALF_BEAT_MS"])
        self.assertEqual(45, self.gait.defines["GAIT_DANCE_CYCLES"])
        body = function_body(self.gait.source, "run_product_trick")
        for frame in (
            "s_dance_squat", "s_dance_pop", "s_dance_diagonal_a", "s_dance_diagonal_b",
            "s_dance_left_paw", "s_dance_right_paw",
            "s_dance_hip_left", "s_dance_hip_right",
        ):
            self.assertIn(frame, body)
        self.assertNotIn("s_dance_both_paws", body)
        self.assertIn("recover_dance_home_two_stage", body)

        recovery = function_body(self.gait.source, "recover_dance_home_two_stage", "int")
        self.assertEqual(
            [
                (DANCE_RECOVER_SUPPORT, 250, SMOOTH),
                (HOME, 300, SMOOTH),
            ],
            self.gait.settled_calls(recovery),
        )

    def test_manual_upside_wiggle_uses_slow_small_motion_about_normal_home(self) -> None:
        self.assertEqual(UPSIDE_FEEDBACK_A, self.gait.frame("s_upside_feedback_wiggle_a"))
        self.assertEqual(UPSIDE_FEEDBACK_B, self.gait.frame("s_upside_feedback_wiggle_b"))
        self.assertEqual(350, self.gait.defines["GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS"])
        for frame in (UPSIDE_FEEDBACK_A, UPSIDE_FEEDBACK_B):
            for actual, home in zip(frame, HOME):
                self.assertLessEqual(abs(actual - home), 60)

        # CH0/CH2 are physically reversed while CH1/CH3 are positive.  Thus
        # opposite pulse deltas make each diagonal pair move together.
        for frame in (UPSIDE_FEEDBACK_A, UPSIDE_FEEDBACK_B):
            delta = tuple(actual - home for actual, home in zip(frame, HOME))
            self.assertEqual(delta[0], -delta[3])  # left-front + right-rear
            self.assertEqual(delta[1], -delta[2])  # right-front + left-rear
        body = function_body(self.gait.source, "run_upside_wiggle")
        self.assertIn("sync_roll_home_from_disabled(ROLL_FACE_NORMAL)", body)
        self.assertRegex(body, r"play_frame(?:_settled)?\s*\(\s*s_pose_stand")
        self.assertIn("GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS", body)

    def test_finished_roll_reverse_landing_wiggles_feet_up(self) -> None:
        body = function_body(self.gait.source, "run_roll")
        self.assertIn("s_upside_feedback_wiggle_a", body)
        self.assertIn("s_upside_feedback_wiggle_b", body)
        self.assertRegex(body, r"play_frame(?:_settled)?\s*\(\s*s_pose_stand")
        self.assertNotIn("s_roll_reverse_wiggle", body)
        # Continuous flips retain their verified reverse support between halves.
        half = function_body(self.gait.source, "run_normal_half_roll", "int")
        self.assertIn("s_pose_reverse_home", half)


if __name__ == "__main__":
    unittest.main()

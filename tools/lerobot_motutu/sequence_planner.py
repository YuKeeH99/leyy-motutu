from __future__ import annotations

import re
from typing import Any, Mapping

from .motutu_web_robot import JOINT_KEYS


def clamp(value: float, minimum: float, maximum: float) -> float:
    return max(minimum, min(maximum, value))


def speed_phase_ms(speed: str) -> int:
    if speed == "slow":
        return 90
    if speed == "fast":
        return 45
    return 65


def speed_turn_ms(speed: str) -> int:
    if speed == "slow":
        return 420
    if speed == "fast":
        return 220
    return 320


def estimate_steps_from_seconds(kind: str, speed: str, seconds: float) -> int:
    phase_ms = speed_phase_ms(speed)
    turn_ms = speed_turn_ms(speed)
    if kind in {"left", "right"}:
        cycle_ms = turn_ms * 2
    elif kind in {"spin_left", "spin_right"}:
        cycle_ms = turn_ms * 10
    else:
        cycle_ms = phase_ms * 12
    return max(1, int(round(float(seconds) * 1000.0 / max(1, cycle_ms))))


def prompt_to_plan(text: str, defaults: Mapping[str, Any] | None = None) -> dict[str, Any] | None:
    defaults = defaults or {}
    text = str(text or "").strip()
    if not text:
        return None

    kind = str(defaults.get("kind", "home"))
    append_home = bool(defaults.get("append_home", False))
    if re.search(r"翻跟斗|翻滚|打滚", text):
        kind = "flip"
    elif re.search(r"原地.*左.*旋|左.*原地.*旋|原地左转|左旋|左自转|左转圈", text):
        kind = "spin_left"
    elif re.search(r"原地.*右.*旋|右.*原地.*旋|原地右转|右旋|右自转|右转圈", text):
        kind = "spin_right"
    elif re.search(r"握手", text):
        kind = "shake_left" if re.search(r"左前|左手|左", text) else "shake_right"
    elif re.search(r"招手|挥手", text):
        kind = "wave_left" if re.search(r"左前|左手|左", text) else "wave_right"
    elif re.search(r"发球|网球", text):
        kind = "serve_left" if re.search(r"左前|左手|左", text) else "serve_right"
    elif re.search(r"前进|向前|走", text):
        kind = "walk"
    elif re.search(r"后退|向后|后撤", text):
        kind = "back"
    elif re.search(r"左转", text):
        kind = "left"
    elif re.search(r"右转", text):
        kind = "right"
    elif re.search(r"点头", text):
        kind = "nod"
    elif re.search(r"摆|摇", text):
        kind = "swing"
    elif re.search(r"趴|低", text):
        kind = "down"
    elif re.search(r"站", text):
        kind = "stand"
    elif re.search(r"回正|回中|归中|归位|复位|初始位置|中位", text):
        kind = "home"

    if kind != "home" and re.search(r"回正|回中|归中|归位|复位|初始位置|中位", text):
        append_home = True

    speed = str(defaults.get("speed", "normal"))
    if re.search(r"慢|slow", text):
        speed = "slow"
    elif re.search(r"快|fast", text):
        speed = "fast"

    steps = int(defaults.get("steps", 3))
    amplitude = int(defaults.get("amplitude", 12))
    recover = int(defaults.get("recover", 6))
    pause_ms = int(defaults.get("pause_ms", 0))

    steps_match = re.search(r"(\d+)\s*步", text)
    seconds_match = re.search(r"(\d+(?:\.\d+)?)\s*秒", text)
    if steps_match:
        steps = int(steps_match.group(1))
    elif seconds_match:
        steps = estimate_steps_from_seconds(kind, speed, float(seconds_match.group(1)))
    amp_match = re.search(r"(\d+)\s*度", text)
    if amp_match:
        amplitude = int(amp_match.group(1))
    if re.search(r"小摆幅|轻微|柔和", text):
        amplitude = min(amplitude, 8)
    if re.search(r"大摆幅|夸张|用力", text):
        amplitude = max(amplitude, 16)

    return {
        "kind": kind,
        "speed": speed,
        "steps": max(1, steps),
        "amplitude": max(4, min(24, amplitude)),
        "recover": max(4, min(16, recover)),
        "pause_ms": max(0, min(1000, pause_ms)),
        "append_home": append_home,
    }


def build_sequence(plan: Mapping[str, Any], joint_meta: Mapping[str, Mapping[str, Any]] | None = None) -> list[dict[str, Any]]:
    kind = str(plan.get("kind", "home"))
    speed = str(plan.get("speed", "normal"))
    steps = int(plan.get("steps", 1))
    amplitude = int(plan.get("amplitude", 12))
    recover = int(plan.get("recover", 6))
    pause_ms = int(plan.get("pause_ms", 0))

    if kind in {"home", "stand", "down", "nod", "swing", "flip", "wave_left", "wave_right", "shake_left", "shake_right", "spin_left", "spin_right", "serve_left", "serve_right"}:
        return _build_simple_template(kind, speed, steps, amplitude, pause_ms, joint_meta)
    if kind in {"walk", "back", "left", "right"}:
        return _build_wave_sequence(kind, speed, steps, amplitude, recover, pause_ms, joint_meta)
    raise ValueError(f"unsupported plan kind: {kind}")


def _joint_dir(joint_meta: Mapping[str, Mapping[str, Any]] | None, key: str) -> int:
    if joint_meta and key in joint_meta:
        return int(joint_meta[key].get("dir", 1))
    return -1 if key in {"rf", "rr"} else 1


def _abs_range(joint_meta: Mapping[str, Mapping[str, Any]] | None, key: str) -> tuple[float, float]:
    if joint_meta and key in joint_meta:
        item = joint_meta[key]
        return float(item.get("min_abs_deg", 0.0)), float(item.get("max_abs_deg", 180.0))
    return 0.0, 180.0


def _center_abs_deg(joint_meta: Mapping[str, Mapping[str, Any]] | None, key: str) -> float:
    if joint_meta and key in joint_meta:
        return float(joint_meta[key].get("center_abs_deg", 90.0))
    return 90.0


def _logical_to_abs(logical: float, key: str, joint_meta: Mapping[str, Mapping[str, Any]] | None) -> float:
    minimum, maximum = _abs_range(joint_meta, key)
    return clamp(_center_abs_deg(joint_meta, key) + _joint_dir(joint_meta, key) * float(logical), minimum, maximum)


def _frame_from_logical(
    name: str,
    logical: Mapping[str, float],
    move_ms: int,
    pause_ms: int,
    joint_meta: Mapping[str, Mapping[str, Any]] | None,
) -> dict[str, Any]:
    pose = {
        key: round(_logical_to_abs(float(logical.get(key, 0.0)), key, joint_meta), 2)
        for key in JOINT_KEYS
    }
    return {
        "n": name,
        "s": pose,
        "v": max(20, int(move_ms)),
        "d": max(0, int(pause_ms)),
    }


def _forward_wave(sample: int, phase_offset: int, push: int, recover: int) -> int:
    phase = (sample + phase_offset) % 12
    recover_samples = 3
    stance_samples = 12 - recover_samples

    if phase < recover_samples:
        return int(round(push - ((push + recover) * phase) / (recover_samples - 1)))
    phase -= recover_samples
    return int(round(-recover + ((push + recover) * phase) / (stance_samples - 1)))


def _build_wave_sequence(
    kind: str,
    speed: str,
    steps: int,
    amplitude: int,
    recover: int,
    pause_ms: int,
    joint_meta: Mapping[str, Mapping[str, Any]] | None,
) -> list[dict[str, Any]]:
    phase_ms = speed_phase_ms(speed)
    turn_ms = speed_turn_ms(speed)
    push = amplitude
    direction = -1 if kind == "back" else 1
    frames: list[dict[str, Any]] = []

    if kind == "left":
        turn_ms += 120
        turn_amp = max(6, amplitude - 2)
        assist_front = max(2, int(round(turn_amp / 4)))
        assist_rear = max(0, int(round(turn_amp / 5)))
        for idx in range(max(1, steps)):
            frames.append(
                _frame_from_logical(
                    f"left-{idx * 2 + 1}",
                    {"lf": -assist_front, "rf": -turn_amp, "lr": assist_rear, "rr": turn_amp},
                    turn_ms,
                    pause_ms,
                    joint_meta,
                )
            )
            frames.append(
                _frame_from_logical(
                    f"left-{idx * 2 + 2}",
                    {"lf": assist_front, "rf": turn_amp, "lr": -assist_rear, "rr": -turn_amp},
                    turn_ms,
                    pause_ms,
                    joint_meta,
                )
            )
        return frames

    if kind == "right":
        turn_ms += 120
        turn_amp = max(6, amplitude - 2)
        assist_front = max(2, int(round(turn_amp / 4)))
        assist_rear = max(0, int(round(turn_amp / 5)))
        for idx in range(max(1, steps)):
            frames.append(
                _frame_from_logical(
                    f"right-{idx * 2 + 1}",
                    {"lf": turn_amp, "rf": assist_front, "lr": -turn_amp, "rr": -assist_rear},
                    turn_ms,
                    pause_ms,
                    joint_meta,
                )
            )
            frames.append(
                _frame_from_logical(
                    f"right-{idx * 2 + 2}",
                    {"lf": -turn_amp, "rf": -assist_front, "lr": turn_amp, "rr": assist_rear},
                    turn_ms,
                    pause_ms,
                    joint_meta,
                )
            )
        return frames

    for step in range(max(1, steps)):
        for sample in range(12):
            logical = {
                "lf": direction * -_forward_wave(sample, 0, push, recover),
                "rf": direction * _forward_wave(sample, 9, push, recover),
                "lr": direction * -_forward_wave(sample, 6, push, recover),
                "rr": direction * _forward_wave(sample, 3, push, recover),
            }
            frames.append(_frame_from_logical(f"{kind}-{step + 1}-{sample + 1}", logical, phase_ms, pause_ms, joint_meta))
    return frames


def _build_simple_template(
    kind: str,
    speed: str,
    steps: int,
    amplitude: int,
    pause_ms: int,
    joint_meta: Mapping[str, Mapping[str, Any]] | None,
) -> list[dict[str, Any]]:
    _ = speed
    _ = steps
    if kind == "home":
        return [_frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 280, 0, joint_meta)]
    if kind == "stand":
        return [_frame_from_logical("stand", {key: 0.0 for key in JOINT_KEYS}, 300, 0, joint_meta)]
    if kind == "down":
        return [_frame_from_logical("down", {"lf": -25, "rf": -25, "lr": 25, "rr": 25}, 300, 0, joint_meta)]
    if kind == "nod":
        return [
            _frame_from_logical("nod-1", {"lf": 10, "rf": 10, "lr": 0, "rr": 0}, 280, 120, joint_meta),
            _frame_from_logical("nod-2", {"lf": -10, "rf": -10, "lr": 0, "rr": 0}, 280, 120, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 260, 0, joint_meta),
        ]
    if kind == "swing":
        return [
            _frame_from_logical(
                "swing-1",
                {"lf": -amplitude, "rf": amplitude, "lr": amplitude, "rr": -amplitude},
                320,
                max(120, pause_ms),
                joint_meta,
            ),
            _frame_from_logical(
                "swing-2",
                {"lf": amplitude, "rf": -amplitude, "lr": -amplitude, "rr": amplitude},
                320,
                max(120, pause_ms),
                joint_meta,
            ),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 260, 0, joint_meta),
        ]
    if kind == "flip":
        return [
            _frame_from_logical("flip-load-1", {"lf": 10, "rf": 10, "lr": -12, "rr": -12}, 140, 0, joint_meta),
            _frame_from_logical("flip-load-2", {"lf": 18, "rf": 18, "lr": -28, "rr": -28}, 160, 0, joint_meta),
            _frame_from_logical("flip-load-3", {"lf": 24, "rf": 24, "lr": -44, "rr": -44}, 160, 0, joint_meta),
            _frame_from_logical("flip-kick-a", {"lf": 28, "rf": 28, "lr": -60, "rr": -60}, 120, 0, joint_meta),
            _frame_from_logical("flip-kick-b", {"lf": 20, "rf": 20, "lr": -54, "rr": -54}, 100, 0, joint_meta),
            _frame_from_logical("flip-kick-c", {"lf": 28, "rf": 28, "lr": -60, "rr": -60}, 120, 0, joint_meta),
            _frame_from_logical("flip-upside", {"lf": 55, "rf": 55, "lr": 55, "rr": 55}, 220, 40, joint_meta),
            _frame_from_logical("flip-funny-1", {"lf": 48, "rf": 48, "lr": 48, "rr": 48}, 120, 0, joint_meta),
            _frame_from_logical("flip-funny-2", {"lf": 58, "rf": 58, "lr": 58, "rr": 58}, 120, 0, joint_meta),
            _frame_from_logical("flip-funny-3", {"lf": 50, "rf": 50, "lr": 50, "rr": 50}, 120, 0, joint_meta),
            _frame_from_logical("flip-return-load-1", {"lf": -24, "rf": -24, "lr": 24, "rr": 24}, 140, 0, joint_meta),
            _frame_from_logical("flip-return-load-2", {"lf": -52, "rf": -52, "lr": 52, "rr": 52}, 160, 0, joint_meta),
            _frame_from_logical("flip-return-front-max", {"lf": -60, "rf": -60, "lr": 48, "rr": 48}, 140, 0, joint_meta),
            _frame_from_logical("flip-return-push", {"lf": 18, "rf": 18, "lr": -16, "rr": -16}, 120, 0, joint_meta),
            _frame_from_logical("flip-land", {"lf": -8, "rf": -8, "lr": 0, "rr": 0}, 100, 0, joint_meta),
            _frame_from_logical("flip-rear-settle", {"lf": 0, "rf": 0, "lr": -12, "rr": -12}, 80, 0, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta),
        ]
    if kind == "wave_left":
        return [
            _frame_from_logical("wave-left-preload-1", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("wave-left-preload-2", {"lf": 0, "rf": 0, "lr": -10, "rr": -10}, 200, 80, joint_meta),
            _frame_from_logical("wave-left-lift-1", {"lf": -12, "rf": 0, "lr": -10, "rr": -10}, 160, 100, joint_meta),
            _frame_from_logical("wave-left-lift-2", {"lf": -28, "rf": 0, "lr": -10, "rr": -10}, 180, 120, joint_meta),
            _frame_from_logical("wave-left-peak-1", {"lf": -58, "rf": 0, "lr": -10, "rr": -10}, 180, max(160, pause_ms), joint_meta),
            _frame_from_logical("wave-left-mid", {"lf": -36, "rf": 0, "lr": -10, "rr": -10}, 140, 80, joint_meta),
            _frame_from_logical("wave-left-peak-2", {"lf": -60, "rf": 0, "lr": -10, "rr": -10}, 180, max(160, pause_ms), joint_meta),
            _frame_from_logical("wave-left-settle", {"lf": -22, "rf": 0, "lr": -6, "rr": -6}, 140, 60, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta),
        ]
    if kind == "wave_right":
        return [
            _frame_from_logical("wave-right-preload-1", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("wave-right-preload-2", {"lf": 0, "rf": 0, "lr": -10, "rr": -10}, 200, 80, joint_meta),
            _frame_from_logical("wave-right-lift-1", {"lf": 0, "rf": -12, "lr": -10, "rr": -10}, 160, 100, joint_meta),
            _frame_from_logical("wave-right-lift-2", {"lf": 0, "rf": -28, "lr": -10, "rr": -10}, 180, 120, joint_meta),
            _frame_from_logical("wave-right-peak-1", {"lf": 0, "rf": -58, "lr": -10, "rr": -10}, 180, max(160, pause_ms), joint_meta),
            _frame_from_logical("wave-right-mid", {"lf": 0, "rf": -36, "lr": -10, "rr": -10}, 140, 80, joint_meta),
            _frame_from_logical("wave-right-peak-2", {"lf": 0, "rf": -60, "lr": -10, "rr": -10}, 180, max(160, pause_ms), joint_meta),
            _frame_from_logical("wave-right-settle", {"lf": 0, "rf": -22, "lr": -6, "rr": -6}, 140, 60, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta),
        ]
    if kind == "shake_left":
        return [
            _frame_from_logical("shake-left-preload-1", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("shake-left-preload-2", {"lf": 0, "rf": 0, "lr": -10, "rr": -10}, 200, 80, joint_meta),
            _frame_from_logical("shake-left-lift-1", {"lf": -20, "rf": 0, "lr": -10, "rr": -10}, 140, 80, joint_meta),
            _frame_from_logical("shake-left-lift-2", {"lf": -42, "rf": 0, "lr": -10, "rr": -10}, 160, 100, joint_meta),
            _frame_from_logical("shake-left-peak", {"lf": -58, "rf": 0, "lr": -10, "rr": -10}, 180, 100, joint_meta),
            _frame_from_logical("shake-left-pump-1", {"lf": -46, "rf": 0, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-left-pump-2", {"lf": -58, "rf": 0, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-left-pump-3", {"lf": -44, "rf": 0, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-left-pump-4", {"lf": -58, "rf": 0, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-left-settle", {"lf": -24, "rf": 0, "lr": -6, "rr": -6}, 120, 60, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta),
        ]
    if kind == "shake_right":
        return [
            _frame_from_logical("shake-right-preload-1", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("shake-right-preload-2", {"lf": 0, "rf": 0, "lr": -10, "rr": -10}, 200, 80, joint_meta),
            _frame_from_logical("shake-right-lift-1", {"lf": 0, "rf": -20, "lr": -10, "rr": -10}, 140, 80, joint_meta),
            _frame_from_logical("shake-right-lift-2", {"lf": 0, "rf": -42, "lr": -10, "rr": -10}, 160, 100, joint_meta),
            _frame_from_logical("shake-right-peak", {"lf": 0, "rf": -58, "lr": -10, "rr": -10}, 180, 100, joint_meta),
            _frame_from_logical("shake-right-pump-1", {"lf": 0, "rf": -46, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-right-pump-2", {"lf": 0, "rf": -58, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-right-pump-3", {"lf": 0, "rf": -44, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-right-pump-4", {"lf": 0, "rf": -58, "lr": -10, "rr": -10}, 100, 40, joint_meta),
            _frame_from_logical("shake-right-settle", {"lf": 0, "rf": -24, "lr": -6, "rr": -6}, 120, 60, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta),
        ]
    if kind == "spin_left":
        frames = _build_wave_sequence("left", speed, max(1, steps) * 5, amplitude, 6, pause_ms, joint_meta)
        frames.append(_frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta))
        return frames
    if kind == "spin_right":
        frames = _build_wave_sequence("right", speed, max(1, steps) * 5, amplitude, 6, pause_ms, joint_meta)
        frames.append(_frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 140, 0, joint_meta))
        return frames
    if kind == "serve_left":
        return [
            _frame_from_logical("serve-left-preload", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("serve-left-load", {"lf": -18, "rf": 0, "lr": -10, "rr": -10}, 180, 80, joint_meta),
            _frame_from_logical("serve-left-lift", {"lf": -38, "rf": 0, "lr": -10, "rr": -10}, 160, 80, joint_meta),
            _frame_from_logical("serve-left-peak", {"lf": -56, "rf": 0, "lr": -10, "rr": -10}, 180, 80, joint_meta),
            _frame_from_logical("serve-left-press", {"lf": 10, "rf": 0, "lr": -6, "rr": -6}, 120, max(80, pause_ms), joint_meta),
            _frame_from_logical("serve-left-follow", {"lf": -2, "rf": 0, "lr": -2, "rr": -2}, 80, 40, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 120, 0, joint_meta),
        ]
    if kind == "serve_right":
        return [
            _frame_from_logical("serve-right-preload", {"lf": 0, "rf": 0, "lr": -6, "rr": -6}, 180, 60, joint_meta),
            _frame_from_logical("serve-right-load", {"lf": 0, "rf": -18, "lr": -10, "rr": -10}, 180, 80, joint_meta),
            _frame_from_logical("serve-right-lift", {"lf": 0, "rf": -38, "lr": -10, "rr": -10}, 160, 80, joint_meta),
            _frame_from_logical("serve-right-peak", {"lf": 0, "rf": -56, "lr": -10, "rr": -10}, 180, 80, joint_meta),
            _frame_from_logical("serve-right-press", {"lf": 0, "rf": 10, "lr": -6, "rr": -6}, 120, max(80, pause_ms), joint_meta),
            _frame_from_logical("serve-right-follow", {"lf": 0, "rf": -2, "lr": -2, "rr": -2}, 80, 40, joint_meta),
            _frame_from_logical("home", {key: 0.0 for key in JOINT_KEYS}, 120, 0, joint_meta),
        ]
    raise ValueError(f"unsupported simple template: {kind}")

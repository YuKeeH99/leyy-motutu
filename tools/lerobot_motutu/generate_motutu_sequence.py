from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.lerobot_motutu import MotutuWebRobot, MotutuWebRobotConfig
from tools.lerobot_motutu.sequence_planner import build_sequence, prompt_to_plan
from tools.lerobot_motutu.trajectory_io import resolve_repo_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate Motutu action sequences from templates or natural-language prompts.")
    parser.add_argument("--base-url", help="Motutu web service base URL, required for --run and recommended for joint-aware clamping")
    parser.add_argument("--kind", choices=["home", "stand", "down", "nod", "swing", "flip", "wave_left", "wave_right", "spin_left", "spin_right", "serve_left", "serve_right", "walk", "back", "left", "right"], default="walk", help="High-level motion template")
    parser.add_argument("--speed", choices=["slow", "normal", "fast"], default="normal", help="Motion speed")
    parser.add_argument("--steps", type=int, default=3, help="Loop count or step count")
    parser.add_argument("--amplitude", type=int, default=12, help="Logical swing amplitude in degrees")
    parser.add_argument("--recover", type=int, default=6, help="Recovery amplitude for wave gait")
    parser.add_argument("--pause-ms", type=int, default=0, help="Extra pause per frame")
    parser.add_argument("--prompt", help="Natural-language prompt, e.g. '前进3步 慢速 小摆幅'")
    parser.add_argument("--out", help="Write generated sequence JSON to file")
    parser.add_argument("--run", action="store_true", help="Execute the generated sequence on the device")
    parser.add_argument("--append-home", action="store_true", help="Append a home frame after the generated sequence")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    plan = {
        "kind": args.kind,
        "speed": args.speed,
        "steps": args.steps,
        "amplitude": args.amplitude,
        "recover": args.recover,
        "pause_ms": args.pause_ms,
    }
    if args.prompt:
        parsed = prompt_to_plan(args.prompt, plan)
        if parsed is None:
            raise ValueError("prompt is empty")
        plan = parsed

    robot = None
    joint_meta = None
    try:
        if args.base_url:
            robot = MotutuWebRobot(MotutuWebRobotConfig(id="motutu-web-generator", base_url=args.base_url))
            robot.connect(calibrate=False)
            joint_meta = robot.get_joint_meta()

        frames = build_sequence(plan, joint_meta)
        if args.append_home or bool(plan.get("append_home", False)):
            frames.extend(build_sequence({"kind": "home"}, joint_meta))

        payload = {"a": frames, "meta": {"plan": plan}}
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        print(text)

        if args.out:
            out_path = resolve_repo_path(args.out)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(text + "\n", encoding="utf-8")
            print(f"saved sequence to {out_path}")

        if args.run:
            if robot is None:
                raise ValueError("--run requires --base-url")
            resp = robot.send_sequence(frames)
            print("device_response:", json.dumps(resp, ensure_ascii=False))
        return 0
    finally:
        if robot is not None:
            robot.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.lerobot_motutu import MotutuWebRobot, MotutuWebRobotConfig
from tools.lerobot_motutu.trajectory_io import load_jsonl, load_sequence_file


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Replay a recorded Motutu episode or a sequence JSON file.")
    parser.add_argument("--base-url", required=True, help="Motutu web service base URL")
    parser.add_argument("--episode", help="JSONL episode recorded by record_motutu_episode.py")
    parser.add_argument("--sequence-file", help="Sequence JSON file in Motutu or adapter format")
    parser.add_argument("--speed-scale", type=float, default=1.0, help="Scale factor for move/pause duration")
    parser.add_argument("--show-page", action="store_true", help="Show control page before replay")
    parser.add_argument("--home-at-end", action="store_true", help="Return the robot to home after replay")
    return parser.parse_args()


def build_frames(args: argparse.Namespace) -> list[dict[str, Any]]:
    if args.episode:
        records = load_jsonl(args.episode)
        return [dict(record["action"]) for record in records if "action" in record]
    if args.sequence_file:
        return load_sequence_file(args.sequence_file)
    raise ValueError("provide --episode or --sequence-file")


def scale_frame(frame: dict[str, Any], speed_scale: float) -> dict[str, Any]:
    factor = speed_scale if speed_scale > 0 else 1.0
    if "s" in frame:
        return {
            "s": dict(frame["s"]),
            "v": max(0, int(round(float(frame.get("v", 0)) / factor))),
            "d": max(0, int(round(float(frame.get("d", 0)) / factor))),
        }

    result = dict(frame)
    result["move_ms"] = max(0, int(round(float(result.get("move_ms", 120)) / factor)))
    result["pause_ms"] = max(0, int(round(float(result.get("pause_ms", 0)) / factor)))
    return result


def main() -> int:
    args = parse_args()
    frames = [scale_frame(frame, args.speed_scale) for frame in build_frames(args)]
    config = MotutuWebRobotConfig(id="motutu-web-replay", base_url=args.base_url)
    robot = MotutuWebRobot(config)

    try:
        robot.connect()
        if args.show_page:
            print("show_page:", json.dumps(robot.show_page(), ensure_ascii=False))

        print(f"replaying {len(frames)} frames")
        resp = robot.send_sequence(frames)
        print("sequence_response:", json.dumps(resp, ensure_ascii=False))

        total_wait_ms = 0
        for frame in frames:
            if "s" in frame:
                total_wait_ms += int(frame.get("v", 0)) + int(frame.get("d", 0))
            else:
                total_wait_ms += int(frame.get("move_ms", 120)) + int(frame.get("pause_ms", 0))
        if total_wait_ms > 0:
            time.sleep(total_wait_ms / 1000.0)

        print("observation(after):", json.dumps(robot.get_observation(), ensure_ascii=False))
        if args.home_at_end:
            print("home:", json.dumps(robot.home(), ensure_ascii=False))
        return 0
    finally:
        robot.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())

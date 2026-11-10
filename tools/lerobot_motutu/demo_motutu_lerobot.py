from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.lerobot_motutu import MotutuWebRobot, MotutuWebRobotConfig


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Minimal LeRobot bridge demo for the Motutu web API.")
    parser.add_argument("--base-url", default="http://127.0.0.1:18080", help="Motutu web service base URL")
    parser.add_argument("--move-ms", type=int, default=120, help="Single action move duration in ms")
    parser.add_argument("--pause-ms", type=int, default=0, help="Single action extra pause in ms")
    parser.add_argument("--pose", default="", help="Absolute pose as lf,rf,lr,rr; empty means device home center")
    parser.add_argument("--show-page", action="store_true", help="Ask the device to show the control page")
    parser.add_argument("--home", action="store_true", help="Return the robot to the device home center after the test pose")
    parser.add_argument("--schema", action="store_true", help="Print LeRobot observation and action schema")
    return parser.parse_args()


def parse_pose(text: str) -> dict[str, float]:
    parts = [item.strip() for item in text.split(",") if item.strip()]
    if len(parts) != 4:
        raise ValueError("--pose must contain exactly four comma-separated values")

    return {
        "lf": float(parts[0]),
        "rf": float(parts[1]),
        "lr": float(parts[2]),
        "rr": float(parts[3]),
    }


def main() -> int:
    args = parse_args()
    config = MotutuWebRobotConfig(
        id="motutu-web-demo",
        base_url=args.base_url,
        default_move_ms=args.move_ms,
        default_pause_ms=args.pause_ms,
    )
    robot = MotutuWebRobot(config)

    try:
        robot.connect()
        print("connected")
        print("meta:", json.dumps(robot.refresh_meta(), ensure_ascii=False))
        print("joint_meta:", json.dumps(robot.get_joint_meta(), ensure_ascii=False))
        if args.schema:
            print("observation_features:", json.dumps(robot.observation_features, ensure_ascii=False, default=str))
            print("action_features:", json.dumps(robot.action_features, ensure_ascii=False, default=str))
        print("observation(before):", json.dumps(robot.get_observation(), ensure_ascii=False))

        if args.show_page:
            print("show_page:", json.dumps(robot.show_page(), ensure_ascii=False))

        pose = parse_pose(args.pose) if args.pose.strip() else robot.build_home_action()
        pose["move_ms"] = float(args.move_ms)
        pose["pause_ms"] = float(args.pause_ms)
        print("send_action:", json.dumps(robot.send_action(pose), ensure_ascii=False))
        print("observation(after):", json.dumps(robot.get_observation(), ensure_ascii=False))

        if args.home:
            print("home:", json.dumps(robot.home(), ensure_ascii=False))

        return 0
    finally:
        robot.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())

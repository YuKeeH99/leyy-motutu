from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import torch
from lerobot.configs import PreTrainedConfig
from lerobot.policies import get_policy_class, make_pre_post_processors
from lerobot.policies.utils import make_robot_action, prepare_observation_for_inference


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.lerobot_motutu import MotutuWebRobot, MotutuWebRobotConfig
from tools.lerobot_motutu.lerobot_dataset_bridge import (
    build_camera_configs,
    build_dataset_features,
    build_observation_frame,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a pretrained LeRobot policy on Motutu.")
    parser.add_argument("--base-url", required=True, help="Motutu web service base URL")
    parser.add_argument("--policy-path", required=True, help="Local pretrained policy dir or Hub repo id")
    parser.add_argument("--task", default="motutu motion", help="Task string passed into the policy")
    parser.add_argument("--device", default="cpu", help="Inference device, e.g. cpu or cuda")
    parser.add_argument("--steps", type=int, default=20, help="Maximum control steps to run")
    parser.add_argument("--interval-ms", type=int, default=120, help="Fallback wait when policy action lacks timing")
    parser.add_argument("--default-move-ms", type=int, default=120, help="Default move duration if policy omits it")
    parser.add_argument("--default-pause-ms", type=int, default=0, help="Default pause duration if policy omits it")
    parser.add_argument(
        "--camera",
        action="append",
        default=[],
        help="OpenCV camera spec: name=index,width,height,fps",
    )
    parser.add_argument("--show-page", action="store_true", help="Show control page before rollout")
    parser.add_argument("--home-at-end", action="store_true", help="Return robot to home after rollout")
    parser.add_argument("--print-observation", action="store_true", help="Print raw observation keys each step")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_configs = build_camera_configs(args.camera)
    robot = MotutuWebRobot(
        MotutuWebRobotConfig(
            id="motutu-web-policy",
            base_url=args.base_url,
            default_move_ms=args.default_move_ms,
            default_pause_ms=args.default_pause_ms,
            cameras=camera_configs or None,
        )
    )

    try:
        robot.connect()
        if args.show_page:
            print("show_page:", json.dumps(robot.show_page(), ensure_ascii=False))

        policy_cfg = PreTrainedConfig.from_pretrained(args.policy_path)
        policy_cls = get_policy_class(policy_cfg.type)
        policy = policy_cls.from_pretrained(args.policy_path, config=policy_cfg)
        dataset_features = build_dataset_features(
            robot.action_features,
            robot.observation_features,
            use_videos=True,
        )
        preprocessor, postprocessor = make_pre_post_processors(
            policy_cfg=policy_cfg,
            pretrained_path=args.policy_path,
            preprocessor_overrides={
                "device_processor": {"device": args.device},
            },
        )

        policy.reset()
        preprocessor.reset()
        postprocessor.reset()
        device = torch.device(args.device)

        for step in range(max(1, int(args.steps))):
            raw_observation = robot.get_observation()
            if args.print_observation:
                print(
                    "observation_keys:",
                    sorted(raw_observation.keys()),
                )

            observation = build_observation_frame(dataset_features, raw_observation)
            observation = prepare_observation_for_inference(
                observation,
                device,
                task=args.task,
                robot_type=robot.name,
            )
            observation = preprocessor(observation)

            with torch.inference_mode():
                action = policy.select_action(observation)
                action = postprocessor(action)

            robot_action = make_robot_action(action, dataset_features)
            robot_action.setdefault("move_ms", float(args.default_move_ms))
            robot_action.setdefault("pause_ms", float(args.default_pause_ms))
            sent = robot.send_action(robot_action)
            print(f"step {step}: {json.dumps(sent, ensure_ascii=False)}")

            wait_ms = max(
                int(args.interval_ms),
                int(sent.get("move_ms", args.default_move_ms)) + int(sent.get("pause_ms", args.default_pause_ms)),
            )
            if wait_ms > 0:
                time.sleep(wait_ms / 1000.0)

        if args.home_at_end:
            print("home:", json.dumps(robot.home(), ensure_ascii=False))
        return 0
    finally:
        robot.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())

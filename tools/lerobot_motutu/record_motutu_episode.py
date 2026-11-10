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
from tools.lerobot_motutu.lerobot_dataset_bridge import (
    build_camera_configs,
    build_transition_frame,
    create_or_resume_dataset,
)
from tools.lerobot_motutu.trajectory_io import load_sequence_file, parse_pose_csv, resolve_repo_path, save_jsonl


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Record a Motutu execution episode into JSONL or LeRobot dataset.")
    parser.add_argument("--base-url", required=True, help="Motutu web service base URL")
    parser.add_argument("--out", help="Output JSONL path")
    parser.add_argument("--sequence-file", help="Sequence JSON file in Motutu or adapter format")
    parser.add_argument("--pose", action="append", default=[], help="Inline pose as lf,rf,lr,rr")
    parser.add_argument("--move-ms", type=int, default=120, help="Default move duration in ms")
    parser.add_argument("--pause-ms", type=int, default=0, help="Default pause duration in ms")
    parser.add_argument("--settle-ms", type=int, default=50, help="Extra settle wait after each frame in ms")
    parser.add_argument("--dataset-repo-id", help="LeRobot dataset repo id, e.g. user/motutu-demo")
    parser.add_argument("--dataset-root", help="Local root for LeRobot dataset")
    parser.add_argument("--dataset-fps", type=int, default=5, help="Logical FPS used when writing LeRobot dataset")
    parser.add_argument("--dataset-task", default="motutu motion", help="Task label stored in LeRobot dataset")
    parser.add_argument("--resume-dataset", action="store_true", help="Append to an existing LeRobot dataset root")
    parser.add_argument("--no-video", action="store_true", help="Store camera frames as images instead of mp4")
    parser.add_argument(
        "--camera",
        action="append",
        default=[],
        help="OpenCV camera spec: name=index,width,height,fps",
    )
    parser.add_argument("--show-page", action="store_true", help="Show control page before recording")
    parser.add_argument("--home-at-end", action="store_true", help="Return the robot to home after recording")
    return parser.parse_args()


def build_frames(args: argparse.Namespace) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []

    if args.sequence_file:
        frames.extend(load_sequence_file(args.sequence_file))

    for pose_text in args.pose:
        frames.append(
            {
                "s": parse_pose_csv(pose_text),
                "v": int(args.move_ms),
                "d": int(args.pause_ms),
            }
        )

    if not frames:
        raise ValueError("provide --sequence-file or at least one --pose")

    return frames


def main() -> int:
    args = parse_args()
    if not args.out and not args.dataset_repo_id:
        raise ValueError("至少提供 --out 或 --dataset-repo-id 其中之一")
    if args.dataset_repo_id and not args.dataset_root:
        raise ValueError("启用 LeRobot dataset 时需要同时提供 --dataset-root")

    frames = build_frames(args)
    camera_configs = build_camera_configs(args.camera)
    config = MotutuWebRobotConfig(
        id="motutu-web-record",
        base_url=args.base_url,
        default_move_ms=args.move_ms,
        default_pause_ms=args.pause_ms,
        cameras=camera_configs or None,
    )
    robot = MotutuWebRobot(config)
    records: list[dict[str, Any]] = []
    started_at = time.time()
    dataset = None
    dataset_features: dict[str, dict] | None = None

    try:
        robot.connect()
        meta = robot.refresh_meta()
        before = robot.get_observation()
        if args.dataset_repo_id:
            dataset, dataset_features = create_or_resume_dataset(
                repo_id=args.dataset_repo_id,
                root=args.dataset_root,
                fps=max(1, int(args.dataset_fps)),
                robot_type=robot.name,
                action_features=robot.action_features,
                observation_features=robot.observation_features,
                use_videos=not args.no_video,
                resume=args.resume_dataset,
            )

        if args.show_page:
            print("show_page:", json.dumps(robot.show_page(), ensure_ascii=False))

        for index, frame in enumerate(frames):
            normalized = robot.normalize_frame(frame)
            sent = robot.send_action(normalized)
            wait_ms = int(normalized["v"]) + int(normalized["d"]) + int(args.settle_ms)
            if wait_ms > 0:
                time.sleep(wait_ms / 1000.0)
            after = robot.get_observation()

            records.append(
                {
                    "episode_id": Path(args.out or args.dataset_repo_id or "motutu-episode").stem,
                    "step": index,
                    "timestamp_s": round(time.time() - started_at, 6),
                    "action": sent,
                    "observation_before": before,
                    "observation_after": after,
                    "meta_url": meta.get("url"),
                }
            )
            if dataset is not None and dataset_features is not None:
                dataset.add_frame(
                    build_transition_frame(
                        dataset_features,
                        observation=after,
                        action=sent,
                        task=args.dataset_task,
                    )
                )
            before = after

        if args.home_at_end:
            robot.home()

        if dataset is not None:
            dataset.save_episode()
            dataset.finalize()
            print(f"saved 1 episode to LeRobot dataset: {Path(args.dataset_root).resolve()}")

        if args.out:
            output_path = resolve_repo_path(args.out)
            save_jsonl(output_path, records)
            print(f"saved {len(records)} steps to {output_path}")
        return 0
    finally:
        if dataset is not None:
            try:
                dataset.finalize()
            except Exception:
                pass
        robot.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())

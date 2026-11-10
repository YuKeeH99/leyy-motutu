from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any

from lerobot.utils.constants import ACTION, OBS_STR
from lerobot.utils.feature_utils import build_dataset_frame, combine_feature_dicts, hw_to_dataset_features


def has_package(name: str) -> bool:
    return importlib.util.find_spec(name) is not None


def require_package(name: str, extra: str) -> None:
    if has_package(name):
        return
    raise RuntimeError(
        f"当前环境缺少 '{name}'，请先安装 lerobot[{extra}] 扩展，例如："
        f" pip install 'lerobot[{extra}]'"
    )


def parse_camera_spec(text: str) -> tuple[str, Any]:
    from lerobot.cameras.opencv import OpenCVCameraConfig

    if "=" not in text:
        raise ValueError("camera 参数格式应为 name=index,width,height,fps")

    name, raw = text.split("=", 1)
    parts = [item.strip() for item in raw.split(",") if item.strip()]
    if len(parts) != 4:
        raise ValueError("camera 参数格式应为 name=index,width,height,fps")

    source = int(parts[0]) if parts[0].isdigit() else Path(parts[0])
    width = int(parts[1])
    height = int(parts[2])
    fps = int(parts[3])
    return name.strip(), OpenCVCameraConfig(
        index_or_path=source,
        width=width,
        height=height,
        fps=fps,
    )


def build_camera_configs(specs: list[str]) -> dict[str, Any]:
    cameras: dict[str, Any] = {}
    for spec in specs:
        name, config = parse_camera_spec(spec)
        cameras[name] = config
    return cameras


def build_dataset_features(
    action_features: dict[str, type | tuple],
    observation_features: dict[str, type | tuple],
    use_videos: bool = True,
) -> dict[str, dict]:
    return combine_feature_dicts(
        hw_to_dataset_features(action_features, ACTION, use_video=use_videos),
        hw_to_dataset_features(observation_features, OBS_STR, use_video=use_videos),
    )


def build_observation_frame(dataset_features: dict[str, dict], observation: dict[str, Any]) -> dict[str, Any]:
    return build_dataset_frame(dataset_features, observation, OBS_STR)


def build_transition_frame(
    dataset_features: dict[str, dict],
    observation: dict[str, Any],
    action: dict[str, Any],
    task: str,
) -> dict[str, Any]:
    frame = {}
    frame.update(build_dataset_frame(dataset_features, observation, OBS_STR))
    frame.update(build_dataset_frame(dataset_features, action, ACTION))
    frame["task"] = task
    return frame


def create_or_resume_dataset(
    *,
    repo_id: str,
    root: str | Path,
    fps: int,
    robot_type: str,
    action_features: dict[str, type | tuple],
    observation_features: dict[str, type | tuple],
    use_videos: bool,
    resume: bool,
):
    require_package("datasets", "dataset")
    from lerobot.datasets.lerobot_dataset import LeRobotDataset

    dataset_features = build_dataset_features(action_features, observation_features, use_videos=use_videos)
    root_path = Path(root)
    if resume and (root_path / "meta" / "info.json").is_file():
        dataset = LeRobotDataset.resume(
            repo_id=repo_id,
            root=root_path,
        )
    else:
        dataset = LeRobotDataset.create(
            repo_id=repo_id,
            fps=fps,
            root=root_path,
            robot_type=robot_type,
            features=dataset_features,
            use_videos=use_videos,
        )
    return dataset, dataset_features

from __future__ import annotations

from dataclasses import dataclass
from functools import cached_property
from typing import Any, Mapping, Sequence

import requests
from lerobot.robots.config import RobotConfig
from lerobot.robots.robot import Robot
from lerobot.types import RobotAction, RobotObservation

try:
    from lerobot.cameras.opencv import OpenCVCamera
except ImportError:
    OpenCVCamera = None


JOINT_KEYS = ("lf", "rf", "lr", "rr")
DEFAULT_ABS_ANGLE = 90.0


@RobotConfig.register_subclass("motutu_web")
@dataclass(kw_only=True)
class MotutuWebRobotConfig(RobotConfig):
    base_url: str = "http://127.0.0.1:18080"
    timeout_s: float = 3.0
    default_move_ms: int = 120
    default_pause_ms: int = 0
    clamp_actions: bool = True
    cameras: dict[str, Any] | None = None
    camera_read_timeout_ms: int = 200


class MotutuWebRobot(Robot):
    config_class = MotutuWebRobotConfig
    name = "motutu_web"

    def __init__(self, config: MotutuWebRobotConfig):
        super().__init__(config)
        self.config = config
        self._session: requests.Session | None = None
        self._meta: dict[str, Any] | None = None
        self._connected = False
        self._cameras: dict[str, Any] = {}

    @cached_property
    def observation_features(self) -> dict[str, type | tuple]:
        features: dict[str, type | tuple] = {
            "ready": float,
            "hold_ms": float,
            "joint_count": float,
        }
        for key in JOINT_KEYS:
            features[key] = float
            features[f"logical_{key}"] = float
        for name, camera_cfg in (self.config.cameras or {}).items():
            height = int(getattr(camera_cfg, "height", 0) or 0)
            width = int(getattr(camera_cfg, "width", 0) or 0)
            if width > 0 and height > 0:
                features[name] = (height, width, 3)
        return features

    @cached_property
    def action_features(self) -> dict[str, type]:
        features = {key: float for key in JOINT_KEYS}
        features["move_ms"] = float
        features["pause_ms"] = float
        return features

    @property
    def joint_keys(self) -> tuple[str, ...]:
        return JOINT_KEYS

    @property
    def joint_count(self) -> int:
        return len(self.joint_keys)

    @property
    def cameras(self) -> dict[str, Any]:
        return self._cameras

    @property
    def is_connected(self) -> bool:
        return self._connected and self._session is not None

    def connect(self, calibrate: bool = True) -> None:
        if self.is_connected:
            return

        self._session = requests.Session()
        self._meta = self._request_json("GET", "/api/dog/meta")
        self._connect_cameras()
        self._connected = True

        if calibrate:
            self.calibrate()

        self.configure()

    @property
    def is_calibrated(self) -> bool:
        return True

    def calibrate(self) -> None:
        return

    def configure(self) -> None:
        self.refresh_meta()

    def refresh_meta(self) -> dict[str, Any]:
        self._meta = self._request_json("GET", "/api/dog/meta")
        return self._meta

    def get_joint_meta(self) -> dict[str, dict[str, Any]]:
        if self._meta is None:
            self.refresh_meta()
        return {
            str(item["key"]): dict(item)
            for item in (self._meta or {}).get("joints", [])
            if isinstance(item, Mapping) and "key" in item
        }

    def get_observation(self) -> RobotObservation:
        status = self._request_json("GET", "/api/dog/status")
        current = status.get("current") or {}
        abs_pose = current.get("abs") or {}
        logical_pose = current.get("logical") or {}
        default_pose = self._default_abs_pose()
        observation: RobotObservation = {
            "ready": 1.0 if status.get("ready", False) else 0.0,
            "hold_ms": float(logical_pose.get("hold_ms", 0.0)),
            "joint_count": float(self.joint_count),
        }

        for key in JOINT_KEYS:
            observation[key] = float(abs_pose.get(key, default_pose[key]))
            observation[f"logical_{key}"] = float(logical_pose.get(key, 0.0))

        for name, image in self._read_cameras().items():
            observation[name] = image

        return observation

    def send_action(self, action: RobotAction) -> RobotAction:
        frame = self.normalize_frame(action)
        pose = frame["s"]
        payload = {
            "a": [
                {
                    "s": pose,
                    "v": int(frame["v"]),
                    "d": int(frame["d"]),
                }
            ]
        }

        resp = self._request_json("POST", "/api/dog/sequence", json=payload)
        if not resp.get("ok", False):
            raise RuntimeError(f"device rejected action: {resp}")

        result = dict(pose)
        result["move_ms"] = float(frame["v"])
        result["pause_ms"] = float(frame["d"])
        return result

    def send_sequence(self, frames: Sequence[RobotAction]) -> dict[str, Any]:
        normalized = [self.normalize_frame(frame) for frame in frames]
        payload = {"a": normalized}
        resp = self._request_json("POST", "/api/dog/sequence", json=payload)
        if not resp.get("ok", False):
            raise RuntimeError(f"device rejected sequence: {resp}")
        return resp

    def disconnect(self) -> None:
        self._disconnect_cameras()
        if self._session is not None:
            self._session.close()

        self._session = None
        self._connected = False

    def show_page(self) -> dict[str, Any]:
        return self._request_json("GET", "/api/dog/show_page")

    def home(self) -> RobotAction:
        return self.send_action(self._default_abs_pose())

    def build_home_action(self, move_ms: int | None = None, pause_ms: int | None = None) -> RobotAction:
        return self._frame_to_result(
            self.normalize_frame(
                {
                    **self._default_abs_pose(),
                    "move_ms": self.config.default_move_ms if move_ms is None else move_ms,
                    "pause_ms": self.config.default_pause_ms if pause_ms is None else pause_ms,
                }
            )
        )

    def normalize_frame(self, action: RobotAction) -> dict[str, Any]:
        return self._normalize_frame(action)

    def _normalize_action(self, action: RobotAction) -> dict[str, float]:
        frame = self._normalize_frame(action)
        return dict(frame["s"])

    def _normalize_frame(self, action: RobotAction) -> dict[str, Any]:
        move_ms = self.config.default_move_ms
        pause_ms = self.config.default_pause_ms

        if isinstance(action, Mapping):
            if "s" in action:
                pose_input = action["s"]
                move_ms = int(float(action.get("v", move_ms)))
                pause_ms = int(float(action.get("d", pause_ms)))
            elif "action" in action:
                pose_input = action["action"]
                move_ms = int(float(action.get("move_ms", move_ms)))
                pause_ms = int(float(action.get("pause_ms", pause_ms)))
            else:
                pose_input = action
                move_ms = int(float(action.get("move_ms", move_ms)))
                pause_ms = int(float(action.get("pause_ms", pause_ms)))
        else:
            pose_input = action

        pose = self._pose_from_input(pose_input)

        if self.config.clamp_actions:
            pose = self._clamp_pose(pose)

        if move_ms < 0:
            move_ms = 0
        if pause_ms < 0:
            pause_ms = 0

        return {
            "s": pose,
            "v": int(move_ms),
            "d": int(pause_ms),
        }

    def _pose_from_input(self, action: Any) -> dict[str, float]:
        if isinstance(action, Mapping):
            return {key: float(action[key]) for key in JOINT_KEYS}
        if isinstance(action, Sequence):
            return {key: float(action[idx]) for idx, key in enumerate(JOINT_KEYS)}
        raise TypeError("action must be a mapping or sequence")

    def _clamp_pose(self, pose: Mapping[str, float]) -> dict[str, float]:
        joint_meta = self.get_joint_meta()
        clamped = {}

        for key in JOINT_KEYS:
            joint = joint_meta.get(key, {})
            min_deg = float(joint.get("min_abs_deg", 0.0))
            max_deg = float(joint.get("max_abs_deg", 180.0))
            value = float(pose[key])
            clamped[key] = max(min_deg, min(max_deg, value))

        return clamped

    def _frame_to_result(self, frame: Mapping[str, Any]) -> RobotAction:
        result = dict(frame["s"])
        result["move_ms"] = float(frame["v"])
        result["pause_ms"] = float(frame["d"])
        return result

    def _default_abs_pose(self) -> dict[str, float]:
        joint_meta = self.get_joint_meta() if self._meta is not None else {}
        return {
            key: float(joint_meta.get(key, {}).get("center_abs_deg", DEFAULT_ABS_ANGLE))
            for key in JOINT_KEYS
        }

    def _request_json(self, method: str, path: str, **kwargs: Any) -> dict[str, Any]:
        if self._session is None:
            raise RuntimeError("robot is not connected")

        url = self.config.base_url.rstrip("/") + path
        response = self._session.request(method, url, timeout=self.config.timeout_s, **kwargs)
        response.raise_for_status()
        return response.json()

    def _connect_cameras(self) -> None:
        if not self.config.cameras:
            return
        if OpenCVCamera is None:
            raise RuntimeError("当前环境缺少 OpenCV camera 支持，无法启用 LeRobot 相机观测")

        for name, camera_cfg in self.config.cameras.items():
            camera = OpenCVCamera(camera_cfg)
            camera.connect()
            self._cameras[name] = camera

    def _disconnect_cameras(self) -> None:
        for camera in self._cameras.values():
            try:
                camera.disconnect()
            except Exception:
                pass
        self._cameras.clear()

    def _read_cameras(self) -> dict[str, Any]:
        images: dict[str, Any] = {}
        timeout_ms = max(1, int(self.config.camera_read_timeout_ms))
        for name, camera in self._cameras.items():
            images[name] = camera.async_read(timeout_ms=timeout_ms)
        return images

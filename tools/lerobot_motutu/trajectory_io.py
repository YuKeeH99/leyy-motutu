from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Iterable

from .motutu_web_robot import JOINT_KEYS

REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_repo_path(path: str | Path) -> Path:
    file_path = Path(path)
    if not file_path.is_absolute():
        file_path = (REPO_ROOT / file_path).resolve()
    return file_path


def parse_pose_csv(text: str) -> dict[str, float]:
    parts = [item.strip() for item in text.split(",") if item.strip()]
    if len(parts) != 4:
        raise ValueError("pose must contain exactly four comma-separated values")

    return {
        "lf": float(parts[0]),
        "rf": float(parts[1]),
        "lr": float(parts[2]),
        "rr": float(parts[3]),
    }


def load_sequence_file(path: str | Path) -> list[dict[str, Any]]:
    file_path = resolve_repo_path(path)
    data = json.loads(file_path.read_text(encoding="utf-8"))

    if isinstance(data, dict):
        if "a" in data:
            frames = data["a"]
        elif "frames" in data:
            frames = data["frames"]
        else:
            raise ValueError("sequence file must contain 'a' or 'frames'")
    elif isinstance(data, list):
        frames = data
    else:
        raise ValueError("sequence file must contain a list or object")

    normalized: list[dict[str, Any]] = []
    for item in frames:
        if not isinstance(item, dict):
            raise ValueError("each frame must be an object")
        if "s" in item:
            frame = dict(item)
        else:
            frame = {
                "s": {key: float(item[key]) for key in JOINT_KEYS},
                "v": int(float(item.get("move_ms", item.get("v", 120)))),
                "d": int(float(item.get("pause_ms", item.get("d", 0)))),
            }
        normalized.append(frame)

    return normalized


def save_jsonl(path: str | Path, records: Iterable[dict[str, Any]]) -> None:
    file_path = resolve_repo_path(path)
    file_path.parent.mkdir(parents=True, exist_ok=True)
    with file_path.open("w", encoding="utf-8", newline="\n") as handle:
        for record in records:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")


def load_jsonl(path: str | Path) -> list[dict[str, Any]]:
    file_path = resolve_repo_path(path)
    records: list[dict[str, Any]] = []
    for line in file_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        records.append(json.loads(line))
    return records

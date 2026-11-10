#!/usr/bin/env python3
"""Rebuild tone.bin atomically from every valid root-level tone MP3."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import tempfile
from pathlib import Path

import romfs


TONE_NAME_RE = re.compile(r"^(?P<tone_id>[0-9]{3})_.+\.mp3$", re.IGNORECASE)


def collect_tones(assets: Path) -> list[Path]:
    if not assets.is_dir():
        raise ValueError(f"tone directory does not exist: {assets}")

    tones: list[Path] = []
    ids: dict[str, str] = {}
    for path in sorted(assets.iterdir(), key=lambda item: item.name.lower()):
        if not path.is_file() or path.suffix.lower() != ".mp3":
            continue
        match = TONE_NAME_RE.fullmatch(path.name)
        if match is None:
            raise ValueError(f"invalid tone filename (expected NNN_name.mp3): {path.name}")
        tone_id = match.group("tone_id")
        if tone_id in ids:
            raise ValueError(
                f"duplicate tone id {tone_id}: {ids[tone_id]} and {path.name}"
            )
        ids[tone_id] = path.name
        tones.append(path)

    if not tones:
        raise ValueError(f"no tone MP3 files found in: {assets}")
    return tones


def repack(assets: Path, output: Path, exclude=(), max_bytes: int = 0) -> int:
    tones = [tone for tone in collect_tones(assets) if tone.name not in exclude]
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="tone-assets-") as directory:
        stage = Path(directory)
        for tone in tones:
            shutil.copy2(tone, stage / tone.name)

        fd, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
        )
        os.close(fd)
        temporary = Path(temporary_name)
        temporary.unlink()
        try:
            romfs.pack_image(str(stage), str(temporary), "tone")
            if max_bytes and temporary.stat().st_size > max_bytes:
                raise ValueError(f"tone image exceeds partition: {temporary.stat().st_size} > {max_bytes}")
            os.replace(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)

    return len(tones)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--max-bytes", type=int, default=0)
    args = parser.parse_args()

    try:
        count = repack(args.assets.resolve(), args.output.resolve(), args.exclude, args.max_bytes)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"packed {count} tone MP3 files -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

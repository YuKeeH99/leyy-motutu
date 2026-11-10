#!/usr/bin/env python3
"""Validate locally generated V2 startup and robot-dance MP3 assets."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TONE_DIR = ROOT / "res/arcs-mini/tone"


def mp3_duration_seconds(path: Path) -> float:
    data = path.read_bytes()
    offset = 0
    samples = 0
    sample_rate_seen = 0
    bitrate_v1 = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0)
    bitrate_v2 = (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0)
    rates = {
        3: (44100, 48000, 32000),
        2: (22050, 24000, 16000),
        0: (11025, 12000, 8000),
    }

    if data[:3] == b"ID3" and len(data) >= 10:
        size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F)
        offset = 10 + size

    while offset + 4 <= len(data):
        header = int.from_bytes(data[offset:offset + 4], "big")
        if (header & 0xFFE00000) != 0xFFE00000:
            offset += 1
            continue
        version = (header >> 19) & 0x3
        layer = (header >> 17) & 0x3
        bitrate_index = (header >> 12) & 0xF
        rate_index = (header >> 10) & 0x3
        padding = (header >> 9) & 0x1
        if version == 1 or layer != 1 or rate_index == 3:
            offset += 1
            continue
        bitrate = (bitrate_v1 if version == 3 else bitrate_v2)[bitrate_index]
        if bitrate == 0:
            offset += 1
            continue
        sample_rate = rates[version][rate_index]
        frame_samples = 1152 if version == 3 else 576
        frame_size = ((144000 if version == 3 else 72000) * bitrate) // sample_rate + padding
        if frame_size <= 4 or offset + frame_size > len(data):
            break
        samples += frame_samples
        sample_rate_seen = sample_rate
        offset += frame_size

    if samples == 0 or sample_rate_seen == 0:
        raise AssertionError(f"no valid MP3 frames found in {path}")
    return samples / sample_rate_seen


class V2MusicAssetsTest(unittest.TestCase):
    def test_power_on_candidates_offer_five_distinct_selectable_styles(self) -> None:
        candidate_dir = TONE_DIR / "power_on_candidates"
        candidates = sorted(candidate_dir.glob("*.mp3"))
        self.assertEqual(5, len(candidates))
        self.assertEqual(5, len({path.read_bytes() for path in candidates}))
        for path in candidates:
            duration = mp3_duration_seconds(path)
            self.assertGreaterEqual(duration, 4.5, path.name)
            self.assertLessEqual(duration, 5.3, path.name)

        source = (ROOT / "tools/generate_v2_music.py").read_text(encoding="utf-8")
        self.assertIn("POWER_ON_CANDIDATE_STYLES", source)

    def test_power_on_music_is_about_five_seconds(self) -> None:
        duration = mp3_duration_seconds(TONE_DIR / "106_power_on.mp3")
        self.assertGreaterEqual(duration, 4.7)
        self.assertLessEqual(duration, 5.3)

    def test_power_on_generator_uses_the_cheerful_major_arrangement(self) -> None:
        source = (ROOT / "tools/generate_v2_music.py").read_text(encoding="utf-8")
        self.assertIn('POWER_ON_STYLE = "cheerful-major-160bpm"', source)

    def test_robot_dance_music_is_about_twelve_seconds(self) -> None:
        path = TONE_DIR / "110_robot_dance.mp3"
        self.assertTrue(path.is_file())
        duration = mp3_duration_seconds(path)
        self.assertGreaterEqual(duration, 11.8)
        self.assertLessEqual(duration, 12.3)

    def test_dance_tone_is_wired_into_the_resource_pack(self) -> None:
        tone_header = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
        merge = (ROOT / "tools/merge_v2_tones.py").read_text(encoding="utf-8")
        self.assertIn("TONE_ID_110 = 110", tone_header)
        self.assertIn('"110_robot_dance.mp3"', merge)

    def test_missing_lingxin_factory_prompt_is_wired_into_the_resource_pack(self) -> None:
        path = TONE_DIR / "111_lingxin_config_required.mp3"
        self.assertTrue(path.is_file())
        duration = mp3_duration_seconds(path)
        self.assertGreaterEqual(duration, 1.0)
        self.assertLessEqual(duration, 5.0)
        tone_header = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
        merge = (ROOT / "tools/merge_v2_tones.py").read_text(encoding="utf-8")
        self.assertIn("TONE_ID_111 = 111", tone_header)
        self.assertIn('"111_lingxin_config_required.mp3"', merge)

    def test_double_click_reset_tone_is_registered(self) -> None:
        tone_header = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
        merge = (ROOT / "tools/merge_v2_tones.py").read_text(encoding="utf-8")
        self.assertIn("TONE_ID_112 = 112", tone_header)
        self.assertIn('"112_im_laying_down.mp3"', merge)
        self.assertTrue((TONE_DIR / "112_im_laying_down.mp3").is_file())


if __name__ == "__main__":
    unittest.main()

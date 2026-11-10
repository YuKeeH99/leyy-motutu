#!/usr/bin/env python3
"""Contracts for build-time software/hardware AEC reference selection."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
KCONFIG = (ROOT / "src/server/Kconfig").read_text(encoding="utf-8")
OVERLAY = (ROOT / "boards/arcs_mini_v2_1/overlay.conf").read_text(encoding="utf-8")
WAKEUP_C = (ROOT / "src/server/lschat_server/wakeup/app_wakeup.c").read_text(encoding="utf-8")
DOC = ROOT / "doc/AEC方案.md"


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b(?:static\s+)?[\w\s*]+\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function: {name}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


class AecReferenceModeContract(unittest.TestCase):
    def test_kconfig_exposes_exclusive_reference_modes(self) -> None:
        self.assertIn("choice AUDIO_AEC_REFERENCE_MODE", KCONFIG)
        self.assertIn("depends on BOARD_ARCS_MINI_V2_1", KCONFIG)
        self.assertIn("config AUDIO_AEC_REFERENCE_SOFTWARE", KCONFIG)
        self.assertIn("config AUDIO_AEC_REFERENCE_HARDWARE", KCONFIG)

    def test_v21_uses_software_reference_with_only_mic1_connected(self) -> None:
        self.assertIn("CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_SINGLE_MIC=y", OVERLAY)
        self.assertIn("CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC=n", OVERLAY)
        self.assertIn("CONFIG_AUDIO_AEC_REFERENCE_SOFTWARE=y", OVERLAY)
        self.assertIn("CONFIG_AUDIO_AEC_REFERENCE_HARDWARE=n", OVERLAY)
        self.assertIn("CONFIG_LISA_AUDIO_PLAY_ECHO_NONE=n", OVERLAY)
        self.assertIn("CONFIG_LISA_AUDIO_PLAY_ECHO_ENABLE=n", OVERLAY)
        self.assertIn("CONFIG_LISA_AUDIO_PLAY_SOFT_ECHO=y", OVERLAY)
        self.assertIn("CONFIG_AUDIO_TDOA_ENABLE=n", OVERLAY)

    def test_invalid_mode_combinations_fail_at_compile_time(self) -> None:
        self.assertIn("Hardware AEC reference cannot use playback soft echo", WAKEUP_C)
        self.assertIn("Hardware AEC reference cannot use DAC echo DMA", WAKEUP_C)
        self.assertIn("Hardware AEC reference cannot use MIC0/MIC1 TDOA", WAKEUP_C)
        self.assertIn("Software AEC reference requires playback soft echo", WAKEUP_C)
        self.assertIn("#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)", WAKEUP_C)

    def test_fusion_selects_the_correct_reference_source(self) -> None:
        body = function_body(WAKEUP_C, "stream_data_fusion")
        self.assertIn("CONFIG_AUDIO_AEC_REFERENCE_HARDWARE", body)
        self.assertIn("algo_buf_in[i].ref1 = adc_data[i].mic2", body)
        self.assertIn("algo_buf_in[i].ref1 = ref_data[i].ref", body)

    def test_runtime_log_identifies_selected_reference(self) -> None:
        init = function_body(WAKEUP_C, "audio_init")
        self.assertIn("AEC reference: hardware analog feedback", init)
        self.assertIn("AEC reference: software playback PCM", init)

    def test_document_explains_switch_and_dma_trap(self) -> None:
        self.assertTrue(DOC.exists(), "doc/AEC方案.md must exist")
        document = DOC.read_text(encoding="utf-8")
        self.assertIn("CONFIG_AUDIO_AEC_REFERENCE_HARDWARE", document)
        self.assertIn("CONFIG_AUDIO_AEC_REFERENCE_SOFTWARE", document)
        self.assertIn("CONFIG_LISA_AUDIO_PLAY_ECHO_ENABLE", document)
        self.assertIn(".\\build.ps1", document)


if __name__ == "__main__":
    unittest.main()

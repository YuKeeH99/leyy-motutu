#!/usr/bin/env python3
"""Host regression tests for device-query speech and IMU-gated touch actions."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "apps/arcs-mini/services"
MCP_TOOLS = ROOT / "apps/arcs-mini/mcp-tools"


class PolicyContractTest(unittest.TestCase):
    def compile_and_run(self, test_name: str, sources: list[str]) -> None:
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        with tempfile.TemporaryDirectory() as temp_dir:
            binary = Path(temp_dir) / test_name
            command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(SERVICES),
                str(ROOT / f"tools/host_tests/{test_name}.c"),
                *[str(SERVICES / source) for source in sources],
                "-o",
                str(binary),
            ]
            subprocess.run(command, cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_voice_query_policy(self) -> None:
        self.compile_and_run("test_voice_query_policy", ["service_voice_query_policy.c"])

    def test_touch_action_policy(self) -> None:
        self.compile_and_run("test_v2_touch_action_policy", ["service_v2_touch_action_policy.c"])

    def test_firmware_integration_contract(self) -> None:
        action_source = (SERVICES / "service_voice_action.c").read_text(encoding="utf-8")
        touch_source = (SERVICES / "service_v2_touch_action.c").read_text(encoding="utf-8")
        imu_source = (SERVICES / "service_v2_imu_interaction.c").read_text(encoding="utf-8")
        device_info_source = (MCP_TOOLS / "mcp_tool_device_info.c").read_text(encoding="utf-8")

        self.assertIn("service_voice_query_from_cloud_code", action_source)
        # Existing cloud deployments deliver the three queries as action_list.
        # They must run on a 12 KiB worker (the same proven budget as the shell
        # text command), leave the eBus callback first, and fully close the old
        # voice chat before starting result TTS followed by normal voice input.
        self.assertIn("voice_query_task", action_source)
        self.assertIn("VOICE_QUERY_TASK_STACK   (12U * 1024U)", action_source)
        self.assertIn("lingxin_server_chat_stop", action_source)
        self.assertIn("lingxin_server_chat_is_active", action_source)
        self.assertIn("LINGXIN_DIALOG_PHASE_IDLE", action_source)
        self.assertIn("lingxin_server_chat_start_query_text", action_source)
        self.assertIn("voice_query_submit(query)", action_source)
        # MCP remains available for newer cloud configurations.
        self.assertIn('"device_code_suffix"', device_info_source)
        self.assertIn("lingxin_server_config_device_code", device_info_source)
        self.assertIn("service_v2_touch_pose_allows_action", touch_source)
        # The upside-down feedback motion remains; only touch is gated.
        self.assertIn("SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE", imu_source)


if __name__ == "__main__":
    unittest.main()

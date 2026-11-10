import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai" / "gcc")) / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
CC = TOOLCHAIN / f"riscv64-unknown-elf-gcc{SUFFIX}"
RUN = TOOLCHAIN / f"riscv64-unknown-elf-run{SUFFIX}"


class VoiceActionPolicyContractTest(unittest.TestCase):
    def compile_and_run_host_test(self, test_name, source_name):
        source_dir = ROOT / "apps/arcs-mini/services"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / f"{test_name}.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / f"tools/host_tests/{test_name}.c"),
                    str(source_dir / source_name),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)

    def test_product_actions_use_fixed_client_timing(self):
        self.compile_and_run_host_test(
            "test_voice_action_policy", "service_voice_action_policy.c"
        )

    def test_multiple_cloud_actions_are_held_in_fifo_order(self):
        self.compile_and_run_host_test(
            "test_voice_action_queue", "service_voice_action_queue.c"
        )

    def test_gait_finished_event_drives_the_next_queued_action(self):
        gait = (ROOT / "apps/arcs-mini/services/service_v2_gait.c").read_text(encoding="utf-8")
        voice = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        messages = (ROOT / "src/framework/voice_msg.h").read_text(encoding="utf-8")
        self.assertIn("VOICE_MSG_V2_GAIT_ACTION_FINISHED", messages)
        self.assertIn("VOICE_MSG_V2_GAIT_ACTION_FINISHED", gait)
        self.assertIn("VOICE_MSG_V2_GAIT_ACTION_FINISHED", voice)
        self.assertIn("service_voice_action_queue_push", voice)
        self.assertIn("service_voice_action_queue_complete", voice)

    def test_v21_motion_actions_are_driven_only_by_cloud_action_list(self):
        source = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        self.assertIn("VOICE_MSG_CLOUD_ACTION_LIST", source)
        self.assertNotIn("voice_action_on_offline", source)
        self.assertNotIn("VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND", source)

    def test_wakeup_keeps_cloud_path_and_does_not_switch_to_esr(self):
        wakeup = (ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8")
        wakeup_api = (ROOT / "src/server/lschat_server/wakeup/app_wakeup.h").read_text(encoding="utf-8")

        self.assertNotIn("app_wakeup_enter_command_mode", wakeup_api)
        self.assertNotIn("app_wakeup_enter_command_mode", wakeup)
        self.assertNotIn("offline command mode", wakeup)


if __name__ == "__main__":
    unittest.main()

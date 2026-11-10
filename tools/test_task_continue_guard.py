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


class TaskContinueGuardTest(unittest.TestCase):
    def test_nested_actions_release_deferred_round_only_once(self):
        source_dir = ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/task_state_strategies"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_task_continue_guard.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_task_continue_guard.c"),
                    str(source_dir / "task_continue_guard.c"),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)

    def test_voice_actions_hold_automatic_continuation_until_fifo_finishes(self):
        strategy = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/task_state_strategies/"
            "state_task_complete_strategies.c"
        ).read_text(encoding="utf-8")
        public_api = (
            ROOT / "third_party/AI2T_LingXinEngine/inc/func/chat_api.h"
        ).read_text(encoding="utf-8")
        voice_action = (
            ROOT / "apps/arcs-mini/services/service_voice_action.c"
        ).read_text(encoding="utf-8")

        self.assertIn("task_continue_guard_should_defer", strategy)
        for name in (
            "voice_chat_action_hold_acquire",
            "voice_chat_action_hold_release",
            "voice_chat_action_hold_cancel",
        ):
            self.assertIn(name, public_api)
            self.assertIn(name, strategy)
            self.assertIn(name, voice_action)


if __name__ == "__main__":
    unittest.main()

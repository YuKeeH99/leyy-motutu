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


class LingxinTtsSuppressContractTest(unittest.TestCase):
    def test_dance_suppresses_the_whole_cloud_tts_response(self):
        adapter_dir = ROOT / "src/server/lingxin_server/adapter"
        source = adapter_dir / "lingxin_tts_suppress_policy.c"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_lingxin_tts_suppress_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(adapter_dir),
                    str(ROOT / "tools/host_tests/test_lingxin_tts_suppress_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)

        action_source = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        adapter_source = (adapter_dir / "lingxin_audio_adapter.c").read_text(encoding="utf-8")
        self.assertIn("lingxin_server_suppress_current_tts", action_source)
        self.assertIn("lingxin_tts_suppress_new_response", adapter_source)
        self.assertIn("lingxin_tts_suppress_should_drop_audio", adapter_source)
        self.assertIn('"suppressed_for_local_action"', adapter_source)


if __name__ == "__main__":
    unittest.main()

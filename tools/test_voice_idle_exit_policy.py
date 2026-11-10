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


class VoiceIdleExitPolicyTest(unittest.TestCase):
    def test_product_default_is_thirty_seconds(self):
        config = (ROOT / "apps/arcs-mini/prj.conf").read_text(encoding="utf-8")
        self.assertIn("CONFIG_CLOUD_IDLE_EXIT_TIMEOUT_MS=30000", config)

    def test_new_activity_invalidates_timer_and_pending_worker(self):
        source_dir = ROOT / "src/category/comm/msgs"
        source = source_dir / "voice_idle_exit_policy.c"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_voice_idle_exit_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_voice_idle_exit_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

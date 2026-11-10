"""Run the actual SNTP worker against radio-off and stale-result boundaries."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""


class NetworkProbeSleepTest(unittest.TestCase):
    def test_running_probe_cancels_without_retry_or_stale_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name in ("lisa_log.h", "lisa_sntp.h", "lisa_thread.h", "lisa_semaphore.h",
                         "lisa_time.h", "voice_msg.h", "service_offline_action.h"):
                (tmp / name).write_text('#include "network_probe_stub.h"\n', encoding="utf-8")
            elf = tmp / "probe.elf"
            subprocess.run([
                str(BIN / ("riscv64-unknown-elf-gcc" + SUFFIX)), "--specs=sim.specs",
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-DCONFIG_BOARD_ARCS_MINI_V2_1=1",
                "-I", str(tmp), "-I", str(ROOT / "tools/host_tests"),
                str(ROOT / "tools/host_tests/test_network_probe_sleep.c"), "-o", str(elf),
            ], check=True, cwd=ROOT)
            for scenario in range(18):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(BIN / ("riscv64-unknown-elf-run" + SUFFIX)),
                                    str(elf), str(scenario)], check=True, cwd=ROOT, timeout=10)


if __name__ == "__main__":
    unittest.main()

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
EXT = ".exe" if os.name == "nt" else ""


class WifiRadioPowerTest(unittest.TestCase):
    def test_radio_transitions_are_ordered_idempotent_and_retryable(self):
        source = ROOT / "src/system/sys_wifi_radio_power.c"
        self.assertTrue(source.exists(), "missing radio-off transaction; standby only disconnects Wi-Fi")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "radio.elf"
            subprocess.run([
                str(BIN / ("riscv64-unknown-elf-gcc" + EXT)), "--specs=sim.specs",
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(source.parent),
                str(ROOT / "tools/host_tests/test_sys_wifi_radio_power.c"), str(source),
                "-o", str(elf),
            ], check=True, cwd=ROOT)
            subprocess.run([str(BIN / ("riscv64-unknown-elf-run" + EXT)), str(elf)],
                           check=True, cwd=ROOT, timeout=30)


if __name__ == "__main__":
    unittest.main()

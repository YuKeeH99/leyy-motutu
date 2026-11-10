"""Execute the real manager against deterministic RTOS/HAL scheduling seams."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
EXT = ".exe" if os.name == "nt" else ""


class RadioPauseTest(unittest.TestCase):
    def test_radio_pause_barrier(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "radio_pause.elf"
            subprocess.run([
                str(BIN / ("riscv64-unknown-elf-gcc" + EXT)), "--specs=sim.specs",
                "-march=rv64ima", "-mabi=lp64",
                "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-function",
                "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-pointer-sign",
                "-DCONFIG_WIFI_MGR_THREAD_STACK=1024", "-DCONFIG_WIFI_MGR_THREAD_PRIORITY=5",
                "-DCONFIG_WIFI_MGR_AUTO_CONNECT_TIMEOUT_MS=100",
                "-I", str(MODULE / "test/wifi_manager/radio_pause"),
                "-I", str(MODULE / "test/common/fake_lisa_time"),
                "-I", str(MODULE / "include"),
                str(MODULE / "test/wifi_manager/test_radio_pause.c"),
                str(MODULE / "src/wifi_manager.c"), "-o", str(elf),
            ], check=True)
            subprocess.run([str(BIN / ("riscv64-unknown-elf-run" + EXT)), str(elf)],
                           check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

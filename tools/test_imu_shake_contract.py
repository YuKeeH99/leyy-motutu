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


class ImuShakeDetectorTest(unittest.TestCase):
    def test_independent_large_shake_policy(self):
        source = ROOT / "apps/arcs-mini/services/service_v2_shake_detector.c"
        header = ROOT / "apps/arcs-mini/services/service_v2_shake_detector.h"
        self.assertTrue(source.exists(), "independent shake detector implementation is missing")
        self.assertTrue(header.exists(), "independent shake detector interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_v2_shake_detector.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "apps/arcs-mini/services"),
                    str(ROOT / "tools/host_tests/test_v2_shake_detector.c"),
                    str(source),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=False)
            self.assertEqual(0, result.returncode, "shake detector behavior contract failed")


if __name__ == "__main__":
    unittest.main()

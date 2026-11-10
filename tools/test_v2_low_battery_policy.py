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


class LowBatteryPolicyTest(unittest.TestCase):
    def test_warning_and_shutdown_product_policy(self):
        source = ROOT / "apps/arcs-mini/services/service_v2_low_battery_policy.c"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_v2_low_battery_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "apps/arcs-mini/services"),
                    str(ROOT / "tools/host_tests/test_v2_low_battery_policy.c"),
                    str(source),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=False)
            self.assertEqual(0, result.returncode, "low-battery product policy failed")


if __name__ == "__main__":
    unittest.main()

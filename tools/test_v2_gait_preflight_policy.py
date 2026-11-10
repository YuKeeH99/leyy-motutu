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


class V2GaitPreflightPolicyTest(unittest.TestCase):
    def test_action_classification_drives_shared_upside_recovery(self):
        services = ROOT / "apps/arcs-mini/services"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_v2_gait_preflight_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(services),
                    str(ROOT / "tools/host_tests/test_v2_gait_preflight_policy.c"),
                    str(services / "service_v2_gait_preflight_policy.c"),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

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


class SysWifiReconnectPolicyTest(unittest.TestCase):
    def test_stale_link_forces_reassociation(self) -> None:
        source_dir = ROOT / "src/system"
        source = source_dir / "sys_wifi_reconnect_policy.c"
        self.assertTrue(source.exists(), "missing Wi-Fi reconnect policy implementation")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_sys_wifi_reconnect_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_sys_wifi_reconnect_policy.c"),
                    str(source),
                    "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

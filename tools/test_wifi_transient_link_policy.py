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


class WifiTransientLinkPolicyTest(unittest.TestCase):
    def test_short_disconnect_preserves_link_until_recovery_or_timeout(self):
        source_dir = ROOT / "src/system"
        source = source_dir / "sys_wifi_transient_link_policy.c"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_wifi_transient_link_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_wifi_transient_link_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

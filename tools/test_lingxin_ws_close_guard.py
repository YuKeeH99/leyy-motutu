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


class LingxinWsCloseGuardTest(unittest.TestCase):
    def test_only_one_caller_can_own_transport_cleanup(self):
        source_dir = ROOT / "src/server/lingxin_server/adapter"
        source = source_dir / "lingxin_ws_close_guard.c"
        header = source_dir / "lingxin_ws_close_guard.h"
        self.assertTrue(source.exists(), "websocket close guard implementation is missing")
        self.assertTrue(header.exists(), "websocket close guard interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_lingxin_ws_close_guard.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-DLINGXIN_WS_CLOSE_GUARD_HOST_TEST=1",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_lingxin_ws_close_guard.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

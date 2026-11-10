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


class WebsocketPendingPolicyTest(unittest.TestCase):
    def test_eagain_owns_one_pending_frame_until_drained(self):
        source_dir = ROOT / "arcs-sdk/components/lisa_websocket"
        source = source_dir / "lisa_websocket_pending_policy.c"
        header = source_dir / "lisa_websocket_pending_policy.h"
        self.assertTrue(source.exists(), "pending-write policy implementation is missing")
        self.assertTrue(header.exists(), "pending-write policy interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_websocket_pending_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_websocket_pending_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

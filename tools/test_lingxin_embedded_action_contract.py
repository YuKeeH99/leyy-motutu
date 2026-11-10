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


class LingxinEmbeddedActionTest(unittest.TestCase):
    def test_explicit_embedded_actions_and_round_dedupe(self):
        source = ROOT / "src/server/lingxin_server/lingxin_action_compat.c"
        header = ROOT / "src/server/lingxin_server/lingxin_action_compat.h"
        self.assertTrue(source.exists(), "embedded action compatibility implementation is missing")
        self.assertTrue(header.exists(), "embedded action compatibility interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_lingxin_action_compat.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src/server/lingxin_server"),
                    "-I", str(ROOT / "arcs-sdk/modules/cjson"),
                    str(ROOT / "tools/host_tests/test_lingxin_action_compat.c"),
                    str(source),
                    str(ROOT / "arcs-sdk/modules/cjson/cJSON.c"),
                    "-lm", "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

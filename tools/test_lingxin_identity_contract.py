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


class LingxinIdentityPriorityTest(unittest.TestCase):
    def test_only_factory_kv_namespace_is_accepted(self):
        kv_header = (ROOT / "src/middleware/kv/kv_user.h").read_text(encoding="utf-8")
        config_source = (ROOT / "src/server/lingxin_server/lingxin_server_config.c").read_text(encoding="utf-8")
        self.assertIn('KV_KEY_LINGXIN_APP_ID                "user.lingxin.app_id"', kv_header)
        self.assertNotIn("KV_KEY_LINGXIN_APP_ID_COMPAT", kv_header)
        self.assertNotIn("KV_KEY_LINGXIN_SN_COMPAT", kv_header)
        self.assertIn('KV_KEY_LINGXIN_PROVISIONED', kv_header)
        self.assertIn("lingxin_identity_bundle_source", config_source)
        self.assertIn("lingxin_server_config_clear", config_source)

    def test_shell_exposes_factory_config_clear_command(self):
        shell_source = (ROOT / "src/shell/cmd/lingxin.c").read_text(encoding="utf-8")
        self.assertIn('"config-clear"', shell_source)
        self.assertIn("lingxin_server_config_clear", shell_source)

    def test_shell_exposes_hot_reload_for_factory_kv_writes(self):
        config_header = (ROOT / "src/server/lingxin_server/lingxin_server_config.h").read_text(encoding="utf-8")
        shell_source = (ROOT / "src/shell/cmd/lingxin.c").read_text(encoding="utf-8")
        self.assertIn("lingxin_server_config_reload", config_header)
        self.assertIn('"config-reload"', shell_source)
        self.assertIn("lingxin_server_config_reload", shell_source)

    def test_cloud_and_wakeup_require_one_complete_factory_bundle(self):
        config_header = (ROOT / "src/server/lingxin_server/lingxin_server_config.h").read_text(encoding="utf-8")
        platform = (ROOT / "src/category/comm/msgs/voice_platform.c").read_text(encoding="utf-8")
        wakeup = (ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8")
        tone_header = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
        self.assertIn("lingxin_server_config_provisioned", config_header)
        self.assertIn("lingxin_server_config_provisioned", platform)
        self.assertIn("lingxin_server_config_provisioned", wakeup)
        self.assertIn("TONE_ID_111 = 111", tone_header)

    def test_production_identity_is_not_compiled_into_the_firmware(self):
        config = (ROOT / "apps/arcs-mini/prj.conf").read_text(encoding="utf-8")
        for name in ("APP_ID", "LICENSE", "APP_CODE", "DEVICE_CODE", "SN"):
            self.assertIn(f'CONFIG_LINGXIN_{name}=""', config)

    def test_legacy_identity_is_never_selected(self):
        source_dir = ROOT / "src/server/lingxin_server"
        source = source_dir / "lingxin_identity_priority.c"
        header = source_dir / "lingxin_identity_priority.h"
        self.assertTrue(source.exists(), "Lingxin identity priority implementation is missing")
        self.assertTrue(header.exists(), "Lingxin identity priority interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_lingxin_identity_priority.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_lingxin_identity_priority.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

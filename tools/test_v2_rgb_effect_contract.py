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


class V2RgbEffectPolicyTest(unittest.TestCase):
    def test_runtime_service_subscribes_dialogue_events_and_is_built(self):
        service = ROOT / "apps/arcs-mini/services/service_v2_rgb_effect.c"
        cmake = (ROOT / "apps/arcs-mini/services/CMakeLists.txt").read_text(encoding="utf-8")
        main = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        self.assertTrue(service.exists(), "RGB runtime service is missing")
        source = service.read_text(encoding="utf-8")
        for event in (
            "VOICE_MSG_WAKEUP_KEYWORD",
            "VOICE_MSG_CLOUD_SESSION_STARTING",
            "VOICE_MSG_PLAYER_TTS_PLAYING",
            "VOICE_MSG_PLAYER_TTS_STOPED",
            "VOICE_MSG_CLOUD_SESSION_FINISHED",
            "VOICE_MSG_CLOUD_DISCONNECTED",
            "VOICE_MSG_POWER_BATTERY_UPDATE",
            "VOICE_MSG_CLOUD_EMOJI",
            "VOICE_MSG_CLOUD_MCP_EMOJI",
        ):
            self.assertIn(event, source)
        self.assertIn("service_v2_rgb_effect.c", cmake)
        self.assertIn("service_v2_rgb_effect_init()", main)

    def test_dialogue_states_drive_bounded_breathing_colours(self):
        source_dir = ROOT / "apps/arcs-mini/services"
        source = source_dir / "service_v2_rgb_effect_policy.c"
        header = source_dir / "service_v2_rgb_effect_policy.h"
        self.assertTrue(source.exists(), "RGB effect policy implementation is missing")
        self.assertTrue(header.exists(), "RGB effect policy interface is missing")
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_v2_rgb_effect_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_v2_rgb_effect_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()

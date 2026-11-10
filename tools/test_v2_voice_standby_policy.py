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


class V2VoiceStandbyPolicyTest(unittest.TestCase):
    def test_idle_and_manual_entry_are_race_safe(self):
        source_dir = ROOT / "apps/arcs-mini/services"
        source = source_dir / "service_v2_voice_standby_policy.c"
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "test_v2_voice_standby_policy.elf"
            subprocess.run(
                [
                    str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(source_dir),
                    str(ROOT / "tools/host_tests/test_v2_voice_standby_policy.c"),
                    str(source), "-o", str(elf),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(RUN), str(elf)], cwd=ROOT, check=True)

    def test_entry_folds_legs_and_usb_only_blocks_automatic_timeout(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(
            encoding="utf-8"
        )
        begin = source[
            source.index("static void standby_begin_entry") :
            source.index("static int standby_release_servo_torque")
        ]
        task = source[
            source.index("static void standby_task") :
            source.index("static void standby_on_voice")
        ]

        self.assertIn("SERVICE_V2_GAIT_ACTION_STANDBY_FOLD", begin)
        self.assertNotIn("SERVICE_V2_GAIT_ACTION_ASSEMBLY", begin)
        self.assertNotIn("SERVICE_V2_GAIT_ACTION_PRONE", begin)
        self.assertIn("power_is_usb_plugged()", task)
        self.assertIn("automatic_entry_allowed", task)

    def test_entry_releases_pwm_only_after_successful_fold(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(
            encoding="utf-8"
        )
        poll = source[
            source.index("static void standby_poll_entry") :
            source.index("static void standby_process_notifications")
        ]

        self.assertIn("gait.current == SERVICE_V2_GAIT_ACTION_STANDBY_FOLD", poll)
        self.assertIn("gait.last_error == 0", poll)
        self.assertIn("service_v2_voice_standby_policy_mark_entry_failed", poll)
        self.assertLess(
            poll.index("gait.current == SERVICE_V2_GAIT_ACTION_STANDBY_FOLD"),
            poll.index("standby_finish_entry()"),
        )

    def test_network_prompts_obey_standby_background_audio_gate(self):
        source = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        disconnect = source[
            source.index("static bool voice_player_should_skip_disconnect_tone") :
            source.index("static bool voice_player_has_network_link")
        ]
        recovery = source[
            source.index("void voice_player_play_network_recovery_tone") :
            source.index("static void cloud_success_tone_timer_cb")
        ]

        self.assertIn("service_v2_voice_standby_background_audio_suppressed()", disconnect)
        self.assertIn("service_v2_voice_standby_background_audio_suppressed()", recovery)


if __name__ == "__main__":
    unittest.main()

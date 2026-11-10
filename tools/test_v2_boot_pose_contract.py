import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class V2BootPoseContractTest(unittest.TestCase):
    def test_boot_queues_standing_pose_after_gait_init(self):
        source = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        gait_init = source.index("service_v2_gait_init()")
        sensor_init = source.index("service_v2_sensors_init()")
        boot_stand = source.index("SERVICE_V2_GAIT_ACTION_STAND", gait_init)
        self.assertGreater(boot_stand, gait_init)
        self.assertLess(sensor_init, boot_stand)

    def test_double_click_keeps_assembly_reset_and_plays_tone_112(self):
        source = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        match = re.search(
            r"VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK[\s\S]*?SERVICE_V2_GAIT_ACTION_ASSEMBLY"
            r"[\s\S]*?TONE_ID_112[\s\S]*?return;",
            source,
        )
        self.assertIsNotNone(match)
        tone_header = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
        self.assertIn("TONE_ID_112 = 112", tone_header)


if __name__ == "__main__":
    unittest.main()

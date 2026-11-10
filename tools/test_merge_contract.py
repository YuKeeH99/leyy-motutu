"""Static merge-boundary checks, not device/cloud integration tests."""
import re
import tempfile
import unittest
from pathlib import Path

from tools import romfs

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_CODES = ["DOG_STAND_UP","STAND_UP","STAND","GET_UP","DOG_LIE_DOWN","LIE_DOWN","LIEDOWN","PRONE","CROUCH","DOG_MOVE_FORWARD","MOVE_FORWARD","GO_FORWARD","FORWARD","DOG_RUN_FORWARD","RUN_FORWARD","GO_FORWARD_FAST","DOG_MOVE_BACKWARD","MOVE_BACKWARD","GO_BACKWARD","BACKWARD","BACK","DOG_RUN_BACKWARD","RUN_BACKWARD","GO_BACKWARD_FAST","DOG_TURN_LEFT","TURN_LEFT","ROTATE_LEFT","LEFT","DOG_TURN_RIGHT","TURN_RIGHT","ROTATE_RIGHT","RIGHT","DOG_FLIP_ONCE","DOG_FLIP","FLIP_ONCE","ROLL_ONCE","DOG_FLIP_CONTINUOUS","DOG_CONTINUOUS_FLIP","FLIP_CONTINUOUS","CONTINUOUS_FLIP","ROLL_DOUBLE","DOG_TENNIS_SERVE","TENNIS_SERVE","DOG_TENNIS_FOREHAND","TENNIS_FOREHAND","DOG_TENNIS_BACKHAND","TENNIS_BACKHAND","DOG_TENNIS_DOUBLE_BACKHAND","TENNIS_DOUBLE_BACKHAND","DOG_HANDSHAKE","HANDSHAKE","DOG_GREETING","GREETING","DOG_SCRATCH","SCRATCH","DOG_SPIN","SPIN","DOG_SIT_DOWN","SIT_DOWN","DOG_KISS","KISS","DOG_DANCE","DANCE","DOG_JUMP","JUMP","DOG_WIGGLE_BUTT","WIGGLE_BUTT","DOG_STOP","STOP_MOVING","STOP","HALT"]


# Product additions after the original merge, including semantic aliases that
# retain the existing one-/two-segment trajectories.
EXPECTED_CODES += ["DOG_PUSHUP", "PUSHUP", "DOG_PLAY_MUSIC", "DOG_BACKFLIP", "BACKFLIP",
                   "DOG_HANDSTAND", "DOG_FRONTFLIP"]


class MergeContractTest(unittest.TestCase):
    def test_all_source_cloud_action_codes_are_present_exactly_once(self):
        source = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        codes = re.findall(r'\{"([A-Z_]+)", SERVICE_V2_GAIT_ACTION_[A-Z_]+\}', source)
        self.assertCountEqual(codes, EXPECTED_CODES)
        self.assertEqual(len(codes), len(set(codes)))
        self.assertIn('cJSON_GetObjectItem(root, "params")', source)
        self.assertIn('cloud_code_is_double_roll(code) ? 2U : 1U', source)

    def test_volume_actions_are_preserved_and_not_reexecuted_as_motion(self):
        source = (ROOT / "apps/arcs-mini/services/voice_lingxin_action.c").read_text(encoding="utf-8")
        for code in ("INCREASE_VOLUME_BY", "INCREASE_VOLUM_BY", "DECREASE_VOLUME_BY",
                     "DECREASE_VOLUM_BY", "CHANGE_VOLUME_TO", "MAX_VOLUME", "MUTE", "UNMUTE"):
            self.assertIn('"' + code + '"', source)
        self.assertIn("service_voice_action_from_cloud_code", source)
        self.assertIn('cJSON_GetObjectItem(action, "parameters")', source)

    def test_packed_new_tones_match_assets(self):
        with tempfile.TemporaryDirectory() as temp:
            romfs.extract_image(str(ROOT / "res/arcs-mini/tone.bin"), temp)
            for name in ("106_power_on.mp3", "107_power_off.mp3",
                         "108_upside_down.mp3", "109_table_shake.mp3"):
                self.assertEqual((Path(temp) / name).read_bytes(),
                                 (ROOT / "res/arcs-mini/tone" / name).read_bytes())

    def test_packed_wakeup_tones_match_editable_assets(self):
        """Changing a wakeup MP3 must change the resource actually flashed."""
        with tempfile.TemporaryDirectory() as temp:
            romfs.extract_image(str(ROOT / "res/arcs-mini/tone.bin"), temp)
            for name in (
                "000_greeting.mp3",
                "001_greeting.mp3",
                "002_greeting.mp3",
                "003_greeting.mp3",
            ):
                self.assertEqual(
                    (Path(temp) / name).read_bytes(),
                    (ROOT / "res/arcs-mini/tone" / name).read_bytes(),
                    name,
                )


if __name__ == "__main__":
    unittest.main()

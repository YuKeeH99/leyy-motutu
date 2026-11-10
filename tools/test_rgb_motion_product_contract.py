import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RgbMotionProductContractTest(unittest.TestCase):
    def test_physical_rgb_output_has_fifty_percent_product_limit(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_actuators.c").read_text(encoding="utf-8")
        self.assertRegex(source, r"RGB_OUTPUT_SCALE_PERCENT\s+50U")
        wire = source[source.index("static uint32_t rgb_calibration_wire_word") :]
        wire = wire[: wire.index("\n}")]
        self.assertIn("effective_brightness_percent", wire)
        self.assertIn("RGB_OUTPUT_SCALE_PERCENT", wire)

    def test_cloud_codes_and_parameters_are_wired(self):
        source = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        for code in (
            "DOG_HANDSHAKE", "DOG_GREETING", "DOG_SPIN", "DOG_SIT_DOWN",
            "DOG_KISS", "DOG_DANCE", "DOG_JUMP", "DOG_SCRATCH",
        ):
            self.assertIn(f'"{code}"', source)
        for parameter in ("SPEED", "DISTANCE", "TIME", "COUNT", "ANGLE", "HAND", "DIRECTION"):
            self.assertIn(f'"{parameter}"', source)
        self.assertIn("service_v2_gait_submit_speed", source)

    def test_dual_touch_channels_have_independent_actions(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_touch_action.c").read_text(encoding="utf-8")
        self.assertRegex(source, r's_pa09\s*=\s*\{[\s\S]*?LEFT_FRONT_HANDSHAKE')
        self.assertRegex(source, r's_pa13\s*=\s*\{[\s\S]*?RIGHT_FRONT_KICK')
        self.assertIn("touch_action_accept_sample(&s_pa09", source)
        self.assertIn("touch_action_accept_sample(&s_pa13", source)

    def test_imu_drives_rgb_and_safe_gait_requests(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_imu_interaction.c").read_text(encoding="utf-8")
        self.assertIn("service_v2_rgb_effect_notify_dizzy()", source)
        self.assertIn("SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE", source)
        self.assertNotIn("final_supported_face", source)
        self.assertRegex(source, r"if\s*\(!final_busy\s*&&\s*!final_sequence_changed\)")
        header = (ROOT / "apps/arcs-mini/services/service_v2_shake_detector.h").read_text(encoding="utf-8")
        self.assertRegex(header, r"SERVICE_V2_SHAKE_WINDOW_MS\s+5000U")
        self.assertRegex(header, r"SERVICE_V2_SHAKE_ROTATION_DEGREES\s+360U")
        self.assertRegex(header, r"SERVICE_V2_SHAKE_GYRO_DEADBAND_LSB\s+3930U")
        self.assertRegex(header, r"SERVICE_V2_SHAKE_REQUIRED_ROTATIONS\s+1U")
        self.assertRegex(header, r"SERVICE_V2_SHAKE_GYRO_LSB_PER_DPS\s+131U")

    def test_upside_feedback_is_fast_retryable_and_independent_of_shake_quiet_time(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_imu_interaction.c").read_text(encoding="utf-8")
        self.assertRegex(source, r"IMU_INTERACTION_UPSIDE_STABLE_MS\s+300U")
        self.assertRegex(source, r"static\s+bool\s+imu_emit_event")
        process = source[source.index("static void imu_process_sample"):]
        upside = process.index("SERVICE_V2_IMU_POSE_UPSIDE_DOWN")
        quiet = process.index("s_detector.quiet_samples > 0U")
        self.assertLess(upside, quiet, "upside detection must not wait for the shake-only quiet window")
        self.assertRegex(process, r"if\s*\(imu_emit_event\(SERVICE_V2_IMU_EVENT_UPSIDE_DOWN")

    def test_imu_motion_and_prompt_are_dispatched_in_the_same_event(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_imu_interaction.c").read_text(encoding="utf-8")
        emit = source[source.index("static bool imu_emit_event") : source.index("static void imu_process_sample")]
        gait = emit.index("service_v2_gait_submit")
        feedback = emit.index("xQueueSend")
        self.assertLess(gait, feedback)
        self.assertNotIn("voice_intent_top", emit)

    def test_dance_music_waits_until_gait_has_really_started(self):
        voice = (ROOT / "apps/arcs-mini/services/service_voice_action.c").read_text(encoding="utf-8")
        gait = (ROOT / "apps/arcs-mini/services/service_v2_gait.c").read_text(encoding="utf-8")
        dispatch = voice[voice.index("static void voice_action_dispatch"):]
        dispatch = dispatch[:dispatch.index("\n}")]
        marker = "static void voice_action_on_gait_started"
        self.assertIn(marker, voice)
        started = voice[voice.index(marker):]
        started = started[:started.index("\n}")]

        self.assertNotIn("voice_player_try_play_prompt_tone_now_url", dispatch)
        self.assertIn("voice_player_try_play_prompt_tone_now_url", started)
        self.assertIn("VOICE_MSG_V2_GAIT_ACTION_STARTED", gait)

    def test_emotion_downlink_shapes_are_supported(self):
        source = (ROOT / "src/server/lingxin_server/lingxin_server.c").read_text(encoding="utf-8")
        for code in ("happy", "sad", "angry", "fearful", "surprised", "disgusted", "neutral"):
            self.assertIn(f'"{code}"', source)
        self.assertIn('strcmp(type_str, "emotion_list")', source)
        self.assertIn("VOICE_MSG_CLOUD_MCP_EMOJI", source)

    def test_dance_duration_stays_in_requested_range(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_gait.c").read_text(encoding="utf-8")
        half = int(re.search(r"#define\s+GAIT_DANCE_HALF_BEAT_MS\s+(\d+)U", source).group(1))
        cycles = int(re.search(r"#define\s+GAIT_DANCE_CYCLES\s+(\d+)U", source).group(1))
        duration_ms = half * cycles
        self.assertGreaterEqual(duration_ms, 8000)
        self.assertLessEqual(duration_ms, 12000)


if __name__ == "__main__":
    unittest.main()

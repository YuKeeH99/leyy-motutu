import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools import romfs


ROOT = Path(__file__).resolve().parents[1]


class LowBatteryToneAssetTest(unittest.TestCase):
    def test_offline_warning_and_shutdown_prompts_are_packaged(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "tone.bin"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/repack_tone.py"),
                    "--assets",
                    str(ROOT / "res/arcs-mini/tone"),
                    "--output",
                    str(image),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            extracted = Path(directory) / "extracted"
            romfs.extract_image(str(image), str(extracted))
            names = {path.name for path in extracted.iterdir() if path.is_file()}

            self.assertIn("115_low_battery.mp3", names)
            self.assertIn("116_low_battery_shutdown.mp3", names)


if __name__ == "__main__":
    unittest.main()

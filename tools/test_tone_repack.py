import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools import romfs


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "repack_tone.py"


class ToneRepackTest(unittest.TestCase):
    def test_embedded_song_excluded_and_overflow_preserves_previous_image(self):
        from tools.repack_tone import repack
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); assets=root/'assets'; assets.mkdir(); output=root/'tone.bin'
            (assets/'000_greeting.mp3').write_bytes(b'keep')
            (assets/'131_music.mp3').write_bytes(b'music'*10000)
            self.assertEqual(repack(assets,output,exclude=['131_music.mp3'],max_bytes=4096),1)
            previous=output.read_bytes()
            with self.assertRaisesRegex(ValueError,'exceeds partition'):
                repack(assets,output,max_bytes=4096)
            self.assertEqual(output.read_bytes(),previous)

    def test_repack_replaces_stale_image_with_all_root_tones_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "tone"
            assets.mkdir()
            (assets / "000_greeting.mp3").write_bytes(b"new-zero")
            (assets / "059_network_suc.mp3").write_bytes(b"new-fifty-nine")
            (assets / "112_reset.mp3").write_bytes(b"new-one-twelve")
            cache = assets / ".omc"
            cache.mkdir()
            (cache / "999_cache.mp3").write_bytes(b"must-not-be-packed")

            stale = root / "stale"
            stale.mkdir()
            (stale / "999_stale.mp3").write_bytes(b"stale")
            output = root / "tone.bin"
            romfs.pack_image(str(stale), str(output), "tone")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--assets",
                    str(assets),
                    "--output",
                    str(output),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)

            extracted = root / "extracted"
            romfs.extract_image(str(output), str(extracted))
            packed = {path.name: path.read_bytes() for path in extracted.iterdir() if path.is_file()}
            self.assertEqual(
                {
                    "000_greeting.mp3": b"new-zero",
                    "059_network_suc.mp3": b"new-fifty-nine",
                    "112_reset.mp3": b"new-one-twelve",
                },
                packed,
            )


if __name__ == "__main__":
    unittest.main()

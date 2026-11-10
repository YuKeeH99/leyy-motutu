"""Append V2 tones without changing any existing tone-bin entry."""
import argparse
import shutil
import tempfile
from pathlib import Path

import romfs

WAKEUP_TONES = ("000_greeting.mp3", "001_greeting.mp3",
                "002_greeting.mp3", "003_greeting.mp3")
NEW_TONES = ("106_power_on.mp3", "107_power_off.mp3",
             "108_upside_down.mp3", "109_table_shake.mp3",
             "110_robot_dance.mp3", "111_lingxin_config_required.mp3",
             "112_im_laying_down.mp3")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    update_tones = WAKEUP_TONES + NEW_TONES
    for name in update_tones:
        if not (args.assets / name).is_file():
            parser.error(f"missing tone: {name}")
    with tempfile.TemporaryDirectory(prefix="v2-tones-") as temp:
        romfs.extract_image(str(args.base), temp)
        for name in update_tones:
            shutil.copy2(args.assets / name, Path(temp) / name)
        romfs.pack_image(temp, str(args.output), "tone")


if __name__ == "__main__":
    main()

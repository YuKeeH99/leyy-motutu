"""Execute the production battery sampler/UI with external hardware boundaries simulated."""
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


class BatterySamplingTest(unittest.TestCase):
    def test_real_battery_sampler_and_ui(self):
        with tempfile.TemporaryDirectory(prefix="battery-test-") as directory:
            tmp = Path(directory)
            for name in (
                "arcs_ap.h", "lisa_log.h", "board.h", "lisa_gpio.h", "lisa_adc.h",
                "sysutils.h", "lisa_timer.h", "voice_msg.h", "voice_msg_structure.h",
                "lisa_kv.h", "lisa_time.h", "FreeRTOS.h", "task.h", "power/power_manager.h",
            ):
                header = tmp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "battery_stub.h"\n', encoding="utf-8")
            elf = tmp / "battery.elf"
            subprocess.run([
                str(CC), "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(tmp), "-I", str(ROOT / "tools/host_tests"),
                "-I", str(ROOT / "boards/arcs_mini_v2_1"),
                "-I", str(ROOT / "src/middleware/battery"),
                str(ROOT / "tools/host_tests/test_battery_sampling.c"),
                str(ROOT / "src/middleware/battery/battery.c"),
                str(ROOT / "src/middleware/battery/battery_ui.c"),
                "-o", str(elf),
            ], cwd=ROOT, check=True)
            for scenario in (
                "startup", "polarity", "queries", "charging_high", "full_confirm",
                "adc_error", "invalid_raw", "low_shutdown", "usb_stops_shutdown", "low_ui",
                "diagnostics", "full_interrupted", "low_error_resets", "charger_error",
                "no_battery", "overvoltage", "clock_wrap",
                "plug_in_preserves_measured_low",
            ):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(RUN), str(elf), scenario], cwd=ROOT, check=False)
                    self.assertEqual(0, result.returncode, scenario)


if __name__ == "__main__":
    unittest.main()

"""Source boundaries for reversible network diagnostics; not RF validation."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "src/shell/cmd/wifi.c").read_text(encoding="utf-8")


class WifiDiagnosticContract(unittest.TestCase):
    def test_status_is_visible_in_current_shell(self):
        body = SOURCE.split("static int wifi_status(int argc, char **argv)\n{", 1)[1].split("#if CONFIG_WIFI_MANAGER", 1)[0]
        self.assertNotIn('printf(', body)
        self.assertIn('shellPrint(shell, "wifi rssi=', body)
        self.assertIn('shellPrint(shell, "wifi ip=', body)

    def test_power_save_switch_is_explicit_and_reversible(self):
        body = SOURCE.split("static int wifi_power_save(", 1)[1].split("\n#endif", 1)[0]
        self.assertIn('argc != 1', body)
        self.assertIn('strcmp(argv[0], "off")', body)
        self.assertIn('strcmp(argv[0], "dtim")', body)
        self.assertIn('!sys_wifi_is_ready()', body)
        self.assertIn('wifi_ps_mode_set(mode)', body)
        self.assertIn('return ret;', body)
        for forbidden in ('lisa_kv_set', 'sys_wifi_stop', 'wifi_disconnect', 'sys_wifi_save_ap'):
            self.assertNotIn(forbidden, body)


if __name__ == "__main__":
    unittest.main()

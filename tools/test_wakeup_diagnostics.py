"""Source contract checks; these do not replace acoustic hardware tests."""
import unittest
from test_conversation_restore import current, function


class WakeupDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.source = current("src/server/lschat_server/wakeup/app_wakeup.c")

    def test_threshold_validation_precedes_array_access(self):
        code = function(self.source, "app_wakeup_sensitivity_level_set")
        self.assertLess(code.index("(unsigned int)level"), code.index("level_remap[level]"))
        self.assertIn("if (ret == ACOMP_ERR_OK)", code)
        self.assertIn("return ret;", code)

    def test_status_does_not_change_settings(self):
        code = function(self.source, "shell_wakeup_diag")
        self.assertIn('strcmp(argv[1], "level")', code)
        status = code[code.index("struct app_datas *app"):]
        for forbidden in ("lisa_kv_", "acomp_wakeup_", "app_wakeup_sensitivity_level_set"):
            self.assertNotIn(forbidden, status)
        for field in ("input=", "submitted=", "alloc_fail=", "keywords=", "work_mode="):
            self.assertIn(field, status if field != "work_mode=" else code)

    def test_init_stops_on_prepare_or_start_failure(self):
        code = function(self.source, "app_wakeup_init")
        self.assertIn("s_diag_prepare_ret = ret;", code)
        self.assertIn("s_diag_start_ret = ret;", code)
        prepare = code.index("ret = acomp_wakeup_prepare_with_resources")
        start = code.index("ret = acomp_wakeup_start()")
        self.assertIn("return ret;", code[prepare:start])
        self.assertIn("return ret;", code[start:code.index("acomp_wakeup_set_algo_mode", start)])

    def test_default_gain_upload_and_threshold_unchanged(self):
        code = function(self.source, "app_wakeup_init")
        self.assertNotIn("app_wakeup_sensitivity_level_set", code)
        self.assertIn("static app_wakeup_sensitivity_level_e s_sensitivity_level = APP_WAKEUP_SENSITIVITY_LEVEL_2;", self.source)


if __name__ == "__main__":
    unittest.main()

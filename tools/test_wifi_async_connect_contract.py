import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "arcs-sdk/modules/wifi_manager/port/wifi_ops/arcs/wifi_arcs.c"


class WifiAsyncConnectContractTest(unittest.TestCase):
    def test_no_wait_connect_returns_before_event_wait(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("int arcs_wifi_sta_connect(")
        end = text.index("int arcs_wifi_sta_disconnect(", start)
        body = text[start:end]

        no_wait = body.index("timeout_ms == 0U")
        event_wait = body.index("xEventGroupWaitBits")
        self.assertLess(no_wait, event_wait)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "配网" / "ble_provision.py"


def load_client():
    spec = importlib.util.spec_from_file_location("ble_provision", CLIENT)
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load BLE provisioning client")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BleProvisioningContractTest(unittest.TestCase):
    def test_client_fragments_commands_exactly_like_device_protocol(self) -> None:
        client = load_client()
        frames = client.encode_command(client.OP_SSID, b"MHZW-test-network")
        self.assertGreater(len(frames), 1)
        self.assertTrue(all(len(frame) <= 20 for frame in frames))
        self.assertEqual((1, len(frames)), tuple(frames[0][:2]))
        self.assertEqual(client.PREFIX_ID, int.from_bytes(frames[0][3:5], "little"))
        self.assertEqual(client.OP_SSID, int.from_bytes(frames[0][5:7], "little"))

    def test_client_reassembles_device_info_notifications(self) -> None:
        client = load_client()
        payload = b'{"event":"device_info","chip_id":"0011223344556677","sn":"demo-sn"}'
        decoder = client.NotificationDecoder()
        decoded = []
        for frame in client.encode_notification_payload(payload):
            decoded.extend(decoder.feed(frame))
        self.assertEqual(payload, decoded[-1])

    def test_firmware_exposes_device_info_and_mhzw_hotspot(self) -> None:
        header = (ROOT / "arcs-sdk/soc/arcs/hal/modules/profiles/netcfg_ble/netcfg_bles/api/netcfg_ble.h").read_text(encoding="utf-8")
        profile = (ROOT / "arcs-sdk/soc/arcs/hal/modules/profiles/netcfg_ble/netcfg_bles/src/netcfg_bles.c").read_text(encoding="utf-8")
        app = (ROOT / "src/middleware/ble/app_ble_common.c").read_text(encoding="utf-8")
        portal = (ROOT / "src/middleware/netcfg_portal/app_netcfg_portal.c").read_text(encoding="utf-8")
        self.assertIn("NETCFG_BLE_OP_DEVICE_INFO", header)
        self.assertIn("NETCFG_BLE_OP_DEVICE_INFO", profile)
        # Identity JSON fields/escaping are exercised by test_ble_device_info
        # against the production builder (it now uses cJSON, not snprintf).
        self.assertIn("lingxin_server_config_sn()", app)
        self.assertNotIn("KV_KEY_LINGXIN_SN_COMPAT", app)
        self.assertIn('"MHZW-%02X%02X%02X"', portal)

    def test_provisioning_stays_discoverable_until_credentials_succeed(self) -> None:
        app = (ROOT / "src/middleware/ble/app_ble_common.c").read_text(encoding="utf-8")
        portal = (ROOT / "src/middleware/netcfg_portal/app_netcfg_portal.c").read_text(encoding="utf-8")

        self.assertNotIn("PORTAL_IDLE_TIMEOUT_MS", portal)
        self.assertIn("s_ble_netcfg_active", app)
        disconnected = app[app.index("static void app_ble_on_disconnected") :]
        disconnected = disconnected[: disconnected.index("\n}")]
        self.assertIn("app_ble_netcfg_adv_schedule(!preserve_bond)", disconnected)
        self.assertIn("!s_ble_netcfg_paused", disconnected)
        self.assertIn("s_ble_sleep_disconnect_conidx == conidx", disconnected)
        self.assertIn("app_ble_netcfg_complete", portal)

    def test_every_button_triple_click_acknowledges_provisioning(self) -> None:
        main = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        self.assertEqual(2, main.count("app_play_netcfg_enter_prompt()"))

    def test_ble_complete_name_is_xiaoqiuquan_without_dropping_mfg_data(self) -> None:
        app = (ROOT / "src/middleware/ble/app_ble_common.c").read_text(encoding="utf-8")
        config = (ROOT / "apps/arcs-mini/prj.conf").read_text(encoding="utf-8")

        self.assertIn('CONFIG_LISA_BLUETOOTH_DEVICE_NAME="xiaoqiuquan"', config)
        self.assertIn("lisa_bt_get_scan_rsp_data", app)
        self.assertIn("'x', 'i', 'a', 'o', 'q', 'i', 'u', 'q', 'u', 'a', 'n'", app)
        self.assertIn("BLE_AD_MFG_DATA", app)


if __name__ == "__main__":
    unittest.main()

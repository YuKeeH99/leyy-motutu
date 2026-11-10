import importlib.util
from pathlib import Path
import unittest
import argparse
import json
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ble_provision', Path(__file__).with_name('ble_provision.py'))
ble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ble)


class WifiListTests(unittest.TestCase):
    def test_identity_display_keeps_legacy_devices_compatible(self):
        self.assertEqual('device_info: ChipID=0011  SN=sample  固件版本=2.6.3',
                         ble.format_device_info({'event': 'device_info', 'chip_id': '0011',
                                                 'sn': 'sample', 'firmware_version': '2.6.3'}))
        self.assertEqual('device_info: ChipID=0011  SN=(未写入)  固件版本=未提供',
                         ble.format_device_info({'event': 'device_info', 'chip_id': '0011', 'sn': ''}))

    def test_missing_fragment_cannot_complete_another_message(self):
        decoder = ble.NotificationDecoder()
        self.assertEqual([], decoder.feed(b'\x01\x03\x01A'))
        self.assertEqual([], decoder.feed(b'\x03\x03\x01C'))
        self.assertEqual([], decoder.feed(b'\x02\x03\x01B'))
        self.assertEqual([b'OK'], decoder.feed(b'\x01\x01\x02OK'))

    def test_wifi_pages_are_replaced_by_new_scan_and_require_all_pages(self):
        collector = ble.WifiListCollector()
        self.assertFalse(collector.feed({'event': 'wifi_scan', 'scan_id': 1, 'page': 1,
                                        'pages': 2, 'status': 'cached', 'aps': [{'ssid': 'Old'}]}))
        self.assertTrue(collector.feed({'event': 'wifi_scan', 'scan_id': 2, 'page': 1,
                                       'pages': 1, 'status': 'cached', 'aps': [{'ssid': '家', 'rssi': -42, 'secured': True}]}))
        self.assertEqual([{'ssid': '家', 'rssi': -42, 'secured': True}], collector.aps)
        self.assertEqual('cached', collector.status)

    def test_wifi_number_selection_rejects_zero_and_retains_hidden_network(self):
        aps = [{'ssid': '家', 'rssi': -42, 'secured': True}]
        self.assertEqual(('家', True), ble.select_wifi(aps, '1'))
        self.assertEqual((None, None), ble.select_wifi(aps, 'm'))
        for bad in ('0', '-1', '2', 'x'):
            with self.assertRaises(ValueError):
                ble.select_wifi(aps, bad)

    def test_legacy_failure_is_decoded_as_failure_not_timeout(self):
        self.assertEqual('密码错误', ble.wifi_failure_reason(b'\x0a\x01\x02'))


class ProvisionFlowTests(unittest.IsolatedAsyncioTestCase):
    async def test_selects_device_scanned_ssid_and_returns_failure_promptly(self):
        sent = []
        class Scanner:
            @staticmethod
            async def discover(timeout):
                return [SimpleNamespace(name='xiaoqiuquan', address='test')]
        class Client:
            def __init__(self, device):
                self.notify = None
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                pass
            def emit(self, payload):
                for frame in ble.encode_notification_payload(payload):
                    self.notify(None, bytearray(frame))
            async def start_notify(self, uuid, notify):
                self.notify = notify
                self.emit(b'{"event":"device_info","chip_id":"0011","sn":"sample"}')
                self.emit(json.dumps({'event': 'wifi_scan', 'scan_id': 1, 'page': 1, 'pages': 1,
                                     'status': 'cached', 'aps': [{'ssid': 'Home', 'rssi': -40, 'secured': True}]}).encode())
            async def write_gatt_char(self, uuid, frame, response):
                sent.append(frame)
                if frame[5:7] == b'\x10\xa0':
                    self.emit(b'\x0a\x01\x02')
        args = argparse.Namespace(device='test', ssid=None, password='password',
                                  scan_seconds=0.01, timeout=0.1)
        with patch.object(ble, 'BleakClient', Client), patch.object(ble, 'BleakScanner', Scanner), \
             patch('builtins.input', return_value='1'):
            result = await ble.provision(args)
        ssid_frames = [frame for frame in sent if frame[5:7] == b'\x02\xa0']
        self.assertEqual(b'Home', ssid_frames[0][9:])
        self.assertEqual(2, result)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""ARCS-MINI BLE 配网命令行工具。

默认交互流程：选择 BLE 设备 -> 读取 ChipID/SN -> 选择设备扫描的 Wi-Fi -> 等待结果。
依赖：``py -3 -m pip install -r 配网/requirements.txt``。
"""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import struct
from typing import Iterable

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # Allow protocol-only unit tests without bleak installed.
    BleakClient = None
    BleakScanner = None


PREFIX_ID = 0x03E4
OP_START = 0xA001
OP_SSID = 0xA002
OP_PASSWORD = 0xA003
OP_DONE = 0xA010
OP_DEVICE_INFO = 0xA014
OP_WIFI_SCAN = 0xA015

DATA_UUID = "0000e403-0000-1000-8000-00805f9b34fb"
STATUS_UUID = "0000e404-0000-1000-8000-00805f9b34fb"
ATT_MTU_PAYLOAD = 20


def encode_command(opcode: int, payload: bytes = b"") -> list[bytes]:
    """Encode one command using the firmware's 20-byte raw-frame protocol."""
    body = bytes(payload)
    first_capacity = ATT_MTU_PAYLOAD - 3 - 6
    later_capacity = ATT_MTU_PAYLOAD - 3
    remaining = max(0, len(body) - first_capacity)
    count = 1 + (remaining + later_capacity - 1) // later_capacity
    frames: list[bytes] = []

    first = body[:first_capacity]
    head = struct.pack("<HHH", PREFIX_ID, opcode, len(body))
    frames.append(bytes((1, count, len(head) + len(first))) + head + first)
    offset = len(first)
    for index in range(2, count + 1):
        chunk = body[offset: offset + later_capacity]
        frames.append(bytes((index, count, len(chunk))) + chunk)
        offset += len(chunk)
    return frames


def encode_notification_payload(payload: bytes) -> list[bytes]:
    """Mirror the device custom-notification framing (used by tests/tools)."""
    chunks = [payload[i:i + 17] for i in range(0, len(payload), 17)] or [b""]
    total = len(chunks)
    return [bytes((i + 1, total, len(chunk))) + chunk for i, chunk in enumerate(chunks)]


class NotificationDecoder:
    def __init__(self) -> None:
        self._parts: dict[int, bytes] = {}
        self._total = 0
        self._next = 1

    def feed(self, data: bytes) -> list[object]:
        """Return decoded status integers or JSON/bytes messages."""
        raw = bytes(data)
        if len(raw) == 2:
            return [struct.unpack("<H", raw)[0]]
        if len(raw) < 3:
            return [raw]
        index, total, length = raw[:3]
        if index == 0 or total == 0 or index > total or length != len(raw) - 3 or length > 17:
            self._parts.clear()
            self._total = 0
            return [raw]
        if index == 1:
            self._parts.clear()
            self._total = total
            self._next = 1
        if total != self._total or index != self._next:
            self._parts.clear()
            self._total = 0
            return []
        self._parts[index] = raw[3:3 + length]
        self._next = index + 1
        if len(self._parts) != self._total:
            return []
        complete = b"".join(self._parts[i] for i in range(1, self._total + 1))
        self._parts.clear()
        self._total = 0
        return [complete]


class WifiListCollector:
    """Collect whole JSON pages; a new scan never inherits old AP entries."""
    def __init__(self) -> None:
        self.scan_id = None
        self.aps = []
        self.status = 'busy'
        self._pages = {}

    def feed(self, message: dict) -> bool:
        if message.get('event') != 'wifi_scan':
            return False
        page, pages = message.get('page', 0), message.get('pages', 0)
        if not isinstance(page, int) or not isinstance(pages, int) or not 1 <= page <= pages <= 24:
            return False
        scan_id = message.get('scan_id')
        if scan_id != self.scan_id:
            self.scan_id = scan_id
            self._pages = {}
            self.aps = []
        self.status = message.get('status', 'failed')
        self._pages[page] = message.get('aps', [])
        if set(self._pages) != set(range(1, pages + 1)):
            return False
        self.aps = [ap for i in range(1, pages + 1) for ap in self._pages[i]]
        return True


def select_wifi(aps: list, choice: str) -> tuple[str | None, bool | None]:
    if choice.lower() == 'm':
        return None, None
    number = int(choice)
    if not 1 <= number <= len(aps):
        raise ValueError('无效的 Wi-Fi 序号')
    ap = aps[number - 1]
    return ap['ssid'], bool(ap.get('secured', True))


def wifi_failure_reason(message: bytes) -> str | None:
    if len(message) == 3 and message[:2] == b'\x0a\x01':
        return {1: '未找到热点', 2: '密码错误', 3: '未获取到 IP', 255: '连接失败'}.get(message[2], '连接失败')
    return None


async def _write_command(client, opcode: int, payload: bytes = b"") -> None:
    for frame in encode_command(opcode, payload):
        await client.write_gatt_char(DATA_UUID, frame, response=True)
        await asyncio.sleep(0.03)


async def _scan(seconds: float):
    devices = await BleakScanner.discover(timeout=seconds)
    # Give our names priority but retain unnamed devices for recovery/debugging.
    return sorted(devices, key=lambda d: (0 if any(x in (d.name or "").upper()
                                                    for x in ("MHZW", "ARCS", "XIAOQIUQUAN")) else 1,
                                          d.name or "", d.address))


def _choose(devices: Iterable, requested: str | None):
    items = list(devices)
    if requested:
        needle = requested.lower()
        for item in items:
            if needle in (item.address or "").lower() or needle in (item.name or "").lower():
                return item
        raise RuntimeError(f"没有找到匹配设备：{requested}")
    if not items:
        raise RuntimeError("没有扫描到 BLE 设备，请确认设备已进入配网模式")
    print("\n扫描结果：")
    for index, item in enumerate(items, 1):
        print(f"  {index:2d}. {item.name or '(无名称)'}  {item.address}")
    while True:
        try:
            selected = int(input("请选择设备序号：").strip())
            if not 1 <= selected <= len(items):
                raise ValueError('无效的设备序号')
            return items[selected - 1]
        except (ValueError, IndexError):
            print("序号无效，请重新输入。")


def format_device_info(message: dict) -> str:
    return (f"{message.get('event', '消息')}: ChipID={message.get('chip_id', '-')}  "
            f"SN={message.get('sn') or '(未写入)'}  "
            f"固件版本={message.get('firmware_version') or '未提供'}")


async def provision(args: argparse.Namespace) -> int:
    if BleakScanner is None or BleakClient is None:
        raise RuntimeError("缺少 bleak，请执行：py -3 -m pip install -r 配网/requirements.txt")
    devices = await _scan(args.scan_seconds)
    device = _choose(devices, args.device)
    print(f"连接 {device.name or '(无名称)'} ({device.address}) ...")
    decoder = NotificationDecoder()
    identity_event = asyncio.Event()
    wifi_event = asyncio.Event()
    wifi_list = WifiListCollector()
    result_event = asyncio.Event()
    result = {"ok": False}

    def on_notify(_sender, value: bytearray) -> None:
        for message in decoder.feed(bytes(value)):
            if isinstance(message, int):
                print(f"设备状态：0x{message:04X}")
                if message == 0x0104:  # NETCFG_BLE_SUCCESS
                    result["ok"] = True
                    result_event.set()
            else:
                failure = wifi_failure_reason(message)
                if failure:
                    print(f"配网失败：{failure}")
                    result["ok"] = False
                    result_event.set()
                    continue
                try:
                    decoded = json.loads(message.decode("utf-8"))
                except (AttributeError, UnicodeDecodeError, json.JSONDecodeError):
                    print(f"设备消息：{message!r}")
                    continue
                if not isinstance(decoded, dict):
                    continue
                if decoded.get('event') == 'wifi_scan':
                    if wifi_list.feed(decoded):
                        wifi_event.set()
                    continue
                event = decoded.get("event", "消息")
                print(format_device_info(decoded))
                if "chip_id" in decoded:
                    identity_event.set()
                if event == "wifi_connected":
                    result["ok"] = True
                    result_event.set()

    async with BleakClient(device) as client:
        await client.start_notify(DATA_UUID, on_notify)
        await _write_command(client, OP_DEVICE_INFO)
        try:
            await asyncio.wait_for(identity_event.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            print("警告：旧固件未返回 ChipID/SN，仍可继续尝试配网。")

        secured = True
        ssid = args.ssid
        if not ssid:
            # New firmware sends the list after CCC subscription. A015 is an
            # explicit refresh/fallback and does not alter credential state.
            if not wifi_event.is_set():
                await _write_command(client, OP_WIFI_SCAN)
            while True:
                try:
                    await asyncio.wait_for(wifi_event.wait(), timeout=12.0)
                except asyncio.TimeoutError:
                    print('未收到完整热点列表（旧固件或传输超时），可刷新或手动输入。')
                    wifi_list.aps = []
                messages = {
                    'fresh': '设备刚扫描到的 2.4GHz 热点：',
                    'cached': '设备的 2.4GHz 扫描缓存（SoftAP 配网期间刷新仍使用缓存）：',
                    'busy': '无线正在配网或联网，当前无法扫描；显示已有缓存：',
                    'failed': '设备扫描失败；可重试或手动输入，已有缓存如下：',
                    'paused': '设备已休眠，当前无法扫描。',
                }
                print('\n' + messages.get(wifi_list.status, '设备热点列表：'))
                for number, ap in enumerate(wifi_list.aps, 1):
                    print(f"  {number:2d}. {ap['ssid']}  {ap['rssi']} dBm  "
                          f"{'加密' if ap.get('secured', True) else '开放'}")
                choice = (await asyncio.to_thread(input, '选择编号，r 刷新，m 手动输入隐藏 SSID：')).strip()
                if choice.lower() == 'r':
                    wifi_event.clear()
                    await _write_command(client, OP_WIFI_SCAN)
                    continue
                try:
                    ssid, secured = select_wifi(wifi_list.aps, choice)
                except (ValueError, KeyError):
                    print('序号无效，请重新输入。')
                    continue
                if ssid is None:
                    ssid = (await asyncio.to_thread(input, 'Wi-Fi SSID：')).strip()
                break
        password = args.password if args.password is not None else (
            '' if secured is False else await asyncio.to_thread(getpass.getpass, 'Wi-Fi 密码：'))
        if not ssid:
            raise RuntimeError("SSID 不能为空")
        if len(ssid.encode('utf-8')) > 32 or len(password.encode('utf-8')) > 64:
            raise RuntimeError('SSID 最多 32 字节，密码最多 64 字节（UTF-8）')
        print(f"正在配置 Wi-Fi：{ssid}")
        await _write_command(client, OP_START)
        await _write_command(client, OP_SSID, ssid.encode("utf-8"))
        await _write_command(client, OP_PASSWORD, password.encode("utf-8"))
        await _write_command(client, OP_DONE)
        try:
            await asyncio.wait_for(result_event.wait(), timeout=args.timeout)
        except asyncio.TimeoutError:
            print("配网超时：请检查密码、2.4GHz Wi-Fi 信号及设备日志。")
            return 2
        if result['ok']:
            print("配网成功。")
        return 0 if result["ok"] else 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="BLE 名称或地址的部分字符")
    parser.add_argument("--ssid", help="非交互指定 Wi-Fi SSID")
    parser.add_argument("--password", help="非交互指定 Wi-Fi 密码")
    parser.add_argument("--scan-seconds", type=float, default=6.0)
    parser.add_argument("--timeout", type=float, default=25.0)
    args = parser.parse_args()
    try:
        return asyncio.run(provision(args))
    except (RuntimeError, OSError) as exc:
        print(f"失败：{exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

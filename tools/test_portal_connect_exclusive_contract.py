#!/usr/bin/env python3
"""Contracts for keeping saved-AP auto-connect out of portal retries."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PORTAL_C = (ROOT / "src/middleware/netcfg_portal/app_netcfg_portal.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b(?:static\s+)?[\w\s*]+\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function: {name}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


class PortalConnectExclusiveContract(unittest.TestCase):
    def test_each_manual_attempt_cancels_sdk_saved_ap_resume(self) -> None:
        body = function_body(PORTAL_C, "try_connect")
        connect = body.index("sys_network_connect_wifi")
        cancel = body.index("sys_wifi_reconnect_saved_once_stop", connect)
        retry = body.index("PORTAL_CONNECT_RETRY_MS", connect)
        self.assertLess(connect, cancel)
        self.assertLess(cancel, retry)
        self.assertGreaterEqual(body.count("sys_wifi_reconnect_saved_once_stop"), 2)


if __name__ == "__main__":
    unittest.main()

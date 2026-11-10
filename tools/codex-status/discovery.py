"""Discover a paired bridge over IPv4 without exposing its token."""
import asyncio
import hmac
import re
import socket

PORT = 8767
REQUEST = re.compile(rb"LS26(2?)\? ([0-9a-f]{16})")


class Discovery(asyncio.DatagramProtocol):
    def __init__(self, token, port=8765, legacy=True):
        self.port = str(port).encode("ascii")
        self.legacy = legacy
        self.key = token.encode("ascii")

    def datagram_received(self, data, peer):
        match = REQUEST.fullmatch(data)
        if not match or (not match[1] and not self.legacy):
            return
        # A connected UDP socket uses the interface that reaches this board.
        # Sign that address so a response replayed from another host is rejected.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reply:
            try:
                reply.connect(peer)
                address = reply.getsockname()[0].encode("ascii")
                payload = b"LS26" + match[1] + b"! " + match[2] + b" " + address
                if match[1]:
                    payload += b" " + self.port
                mac = hmac.digest(self.key, payload, "sha256").hex().encode("ascii")
                reply.send(payload + b" " + mac)
            except OSError:
                pass

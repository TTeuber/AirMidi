"""The box's JSON API over BLE (firmware/components/ble_config, lib/blecfg.h): the same requests as the web page.

    uv run --with bleak python tools/hil/blecfg.py <ble name> GET /api/status
    uv run --with bleak python tools/hil/blecfg.py <ble name> POST /api/mode '{"mode": "network"}'

As a module: `async with BleConfig(name) as c: status, reply = await c.call("GET", "/api/status")`. The first
request pairs (macOS bonds "just works"); a phone the box does not trust gets ATT error 0x08 unless the pairing
window is open (Setup mode, 2 min after power-on, or POST /api/phones/window from a trusted phone).
"""

import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

BASE = "9a5c0010-3f2b-4d8e-a4b1-6c2e8f1d0a"
SERVICE, REQUEST, RESPONSE = BASE + "00", BASE + "11", BASE + "12"
LAST = 0x01


class BleConfig:
    def __init__(self, name, timeout=20):
        self.name, self.timeout = name, timeout
        self.client, self.next_id = None, 1
        self.pending = {}  # id -> (bytearray, future)

    async def __aenter__(self):
        dev = await BleakScanner.find_device_by_name(self.name, timeout=self.timeout)
        if not dev:
            raise RuntimeError(f"{self.name} not found over BLE")
        self.client = BleakClient(dev)
        await self.client.connect()
        await self.client.start_notify(RESPONSE, self._on_piece)
        return self

    async def __aexit__(self, *exc):
        await self.client.disconnect()

    def _on_piece(self, _, piece):
        piece = bytes(piece)
        if len(piece) < 2 or piece[0] not in self.pending:
            return
        buf, fut = self.pending[piece[0]]
        buf.extend(piece[2:])
        if piece[1] & LAST and not fut.done():
            fut.set_result(bytes(buf))

    async def call(self, method, path, body=None, timeout=10):
        """(status, parsed JSON reply). body: a dict (sent as JSON), bytes or None."""
        rid = self.next_id
        self.next_id = self.next_id % 255 + 1
        data = body if isinstance(body, (bytes, bytearray)) else (json.dumps(body).encode() if body else b"")
        msg = f"{method} {path}\n".encode() + data
        fut = asyncio.get_running_loop().create_future()
        self.pending[rid] = (bytearray(), fut)
        room = self.client.mtu_size - 3 - 2
        try:
            for off in range(0, max(len(msg), 1), room):
                part = msg[off:off + room]
                last = off + room >= len(msg)
                await self.client.write_gatt_char(REQUEST, bytes([rid, LAST if last else 0]) + part, response=True)
            reply = await asyncio.wait_for(fut, timeout)
        finally:
            del self.pending[rid]
        head, _, text = reply.partition(b"\n")
        return int(head), json.loads(text or b"{}")


async def main():
    name, method, path = sys.argv[1:4]
    body = json.loads(sys.argv[4]) if len(sys.argv) > 4 else None
    async with BleConfig(name) as c:
        status, reply = await c.call(method, path, body)
        print(status)
        print(json.dumps(reply, indent=2))


if __name__ == "__main__":
    asyncio.run(main())

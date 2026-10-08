"""A BLE MIDI connection straight from CoreBluetooth (bleak), shaped like a python-rtmidi port pair, so the HIL
tests can run without CoreMIDI's Bluetooth driver. Same bytes on the air; the difference is timing: macOS keeps a
30 ms connection interval for a plain CoreBluetooth client, against 7.5 ms for its MIDI driver (spike S2), so use
CoreMIDI (tools/hil/blemidi) for latency numbers and this for correctness when the driver will not cooperate.

    port = BlePort("airmidi-7530")      # connects, subscribes
    port.send_message([0x90, 60, 100])  # rtmidi MidiOut style, SysEx of any length split into BLE packets
    port.set_callback(lambda ev, data: ...)   # rtmidi MidiIn style: ev = (message, delta)
    port.close()
"""

import asyncio
import threading
import time

from bleak import BleakClient, BleakScanner

MIDI_CHR = "7772e5db-3868-4112-a1a9-f2669d106bf3"


def _ts():
    return int(time.monotonic() * 1000) & 0x1FFF


def packets(msg, max_len):
    """One MIDI message -> BLE MIDI packets (header + timestamps), SysEx split across packets."""
    ts = _ts()
    hdr, tsb = 0x80 | (ts >> 7), 0x80 | (ts & 0x7F)
    if msg[0] != 0xF0:
        return [bytes([hdr, tsb] + list(msg))]
    out, cur = [], [hdr, tsb, 0xF0]
    for b in msg[1:-1]:
        if len(cur) >= max_len:
            out.append(bytes(cur))
            cur = [hdr]
        cur.append(b)
    if len(cur) + 2 > max_len:
        out.append(bytes(cur))
        cur = [hdr]
    out.append(bytes(cur + [tsb, 0xF7]))
    return out


class _Parser:
    """Plain MIDI bytes (timestamps removed) -> whole messages, with running status and SysEx across packets."""

    def __init__(self, emit):
        self.emit, self.status, self.data, self.sysex = emit, 0, [], None

    def feed(self, b):
        if b >= 0xF8:
            self.emit([b])
        elif b == 0xF0:
            self.sysex = [0xF0]
        elif b == 0xF7:
            if self.sysex is not None:
                self.emit(self.sysex + [0xF7])
            self.sysex = None
        elif self.sysex is not None and b < 0x80:
            self.sysex.append(b)
        elif b & 0x80:
            self.sysex, self.status, self.data = None, b, []
            if b in (0xF6,):
                self.emit([b])
                self.status = 0
        elif self.status:
            self.data.append(b)
            need = 1 if (self.status & 0xF0) in (0xC0, 0xD0) or self.status in (0xF1, 0xF3) else 2
            if len(self.data) == need:
                self.emit([self.status] + self.data)
                self.data = []
                if self.status >= 0xF0:
                    self.status = 0


class BlePort:
    def __init__(self, name, timeout=15):
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.callback = None
        self.parser = _Parser(self._emit)
        self.client = self._run(self._connect(name, timeout))

    def _run(self, coro, timeout=30):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    async def _connect(self, name, timeout):
        dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
        if not dev:
            raise RuntimeError(f"{name} not found")
        client = BleakClient(dev)
        await client.connect()
        await client.start_notify(MIDI_CHR, self._on_notify)
        self.max_len = max(20, client.mtu_size - 3)
        self.queue = asyncio.Queue()
        self.loop.create_task(self._writer(client))
        await asyncio.sleep(1.0)  # connection parameters and MTU settle
        return client

    async def _writer(self, client):
        # Like a real BLE MIDI sender: whatever is queued goes out packed into as few packets as fit the MTU
        # (macOS drops writes without response that arrive faster than it can send them).
        while True:
            pkts = [await self.queue.get()]
            while not self.queue.empty():
                pkts.append(self.queue.get_nowait())
            out, cur = [], b""
            for p in pkts:
                body = p[1:]  # drop the header; a continuation packet's body starts with data
                if cur and p[1] & 0x80 and len(cur) + len(body) <= self.max_len:
                    cur += body
                else:
                    if cur:
                        out.append(cur)
                    cur = p
            out.append(cur)
            for p in out:
                await client.write_gatt_char(MIDI_CHR, p, response=False)
                await asyncio.sleep(0.002)

    def _on_notify(self, _, data):
        after_ts = False
        for b in data[1:]:
            if b & 0x80 and not after_ts:
                after_ts = True
                continue
            after_ts = False
            self.parser.feed(b)

    def _emit(self, msg):
        if self.callback:
            self.callback((msg, 0.0), None)

    # rtmidi-like API
    def send_message(self, msg):
        for p in packets(list(msg), self.max_len):
            self.loop.call_soon_threadsafe(self.queue.put_nowait, p)

    def set_callback(self, cb, data=None):
        self.callback = cb

    def cancel_callback(self):
        self.callback = None

    def close_port(self):
        pass

    def close(self):
        try:
            self._run(self.client.disconnect(), timeout=5)
        except Exception:
            pass

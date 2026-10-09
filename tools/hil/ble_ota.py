"""Firmware update over BLE (firmware/components/ble_update): upload a signed image to the box by its BLE name.

    uv run --with bleak python tools/hil/ble_ota.py <ble name> <image.bin>     upload, wait for the restart, print
                                                                              the version the box runs then
    uv run --with bleak python tools/hil/ble_ota.py <ble name> --version      print the running version

As a module: `await upload(name, data)` returns ("accepted" | "failed: <reason>", seconds, bytes/s) and
`await version(name)` the version string.
"""

import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = "9a5c0000-3f2b-4d8e-a4b1-6c2e8f1d0a"
CTRL, DATA, VERSION = BASE + "01", BASE + "02", BASE + "03"
WINDOW = 16 * 1024  # BLE_UPDATE_WINDOW: never more than this beyond the last progress report


def can_send(c):
    """CoreBluetooth drops a write without response its queue has no room for, and bleak does not ask first (3.0:
    peripheralIsReadyToSendWriteWithoutResponse is "not used"); so ask CoreBluetooth ourselves."""
    try:
        return c._backend._peripheral.canSendWriteWithoutResponse()
    except AttributeError:  # another backend
        return True


async def find(name, timeout=20):
    dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
    if not dev:
        raise RuntimeError(f"{name} not found over BLE")
    return dev


async def version(name):
    async with BleakClient(await find(name)) as c:
        return (await c.read_gatt_char(VERSION)).decode()


async def upload(name, data, progress=None):
    async with BleakClient(await find(name)) as c:
        events = asyncio.Queue()
        written = 0
        resend = None

        def on_ctrl(_, b):
            nonlocal written, resend
            if b[0] == 4:  # a piece went missing: go back
                resend = struct.unpack("<I", bytes(b[1:5]))[0]
            if b[0] == 1:
                written = struct.unpack("<I", bytes(b[1:5]))[0]
                if progress:
                    progress(written, len(data))
            events.put_nowait(bytes(b))

        await c.start_notify(CTRL, on_ctrl)
        await c.write_gatt_char(CTRL, struct.pack("<BI", 1, len(data)), response=True)
        ev = await asyncio.wait_for(events.get(), 10)
        if ev[0] == 3:
            return "failed: " + ev[1:].decode(), 0, 0
        chunk = c.mtu_size - 3 - 4  # each piece starts with its offset
        t0 = time.time()
        sent, resends = 0, 0
        while True:
            while sent < len(data):
                if resend is not None:
                    sent, resend, resends = resend, None, resends + 1
                if sent - written >= WINDOW:  # wait for the box's flash to catch up (or to ask for a gap)
                    try:
                        ev = await asyncio.wait_for(events.get(), 2)
                    except asyncio.TimeoutError:
                        resend = written  # pieces went missing and nothing came after them to show it
                        continue
                    if ev[0] == 3:
                        return "failed: " + ev[1:].decode(), 0, 0
                    continue
                n = min(chunk, len(data) - sent, WINDOW - (sent - written))
                while not can_send(c):
                    await asyncio.sleep(0.001)
                await c.write_gatt_char(DATA, struct.pack("<I", sent) + data[sent:sent + n], response=False)
                sent += n
            # Everything sent: wait until it is all written, or the box asks for a gap again.
            while written < len(data) and resend is None:
                try:
                    ev = await asyncio.wait_for(events.get(), 2)
                except asyncio.TimeoutError:
                    resend = written  # the last pieces went missing and nothing came after them to show it
                    break
                if ev[0] == 3:
                    return "failed: " + ev[1:].decode(), 0, 0
            if resend is None:
                break
        await c.write_gatt_char(CTRL, bytes([2]), response=True)
        while True:  # accepted or failed
            ev = await asyncio.wait_for(events.get(), 30)
            if ev[0] in (2, 3):
                break
        took = time.time() - t0
        result = "accepted" if ev[0] == 2 else "failed: " + ev[1:].decode()
        if resends:
            result += f" ({resends} gaps sent again)"
        return result, took, len(data) / took


async def main():
    name = sys.argv[1]
    if sys.argv[2] == "--version":
        print(await version(name))
        return
    data = open(sys.argv[2], "rb").read()
    print(f"before: {await version(name)}")
    result, took, rate = await upload(name, data, lambda w, n: print(f"\r  {w * 100 // n}%", end="", flush=True))
    print(f"\n{result} ({len(data)} bytes in {took:.1f} s, {rate / 1024:.1f} KB/s)")
    if result == "accepted":
        await asyncio.sleep(8)  # restart, then BLE comes up
        print(f"after: {await version(name)}")


if __name__ == "__main__":
    asyncio.run(main())

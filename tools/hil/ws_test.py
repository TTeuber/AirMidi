"""The web UI's MIDI over WebSocket, through the AudioFuse: page -> box -> TRS, and TRS -> box -> page.

    uv run --with websockets --with python-rtmidi python tools/hil/ws_test.py <host>
"""

import asyncio
import statistics
import struct
import sys
import time

import rtmidi
import websockets

host = sys.argv[1]


def port(cls):
    p = cls()
    p.open_port(next(i for i, n in enumerate(p.get_ports()) if "Audiofuse" in n))
    return p


async def main():
    af_out, af_in = port(rtmidi.MidiOut), port(rtmidi.MidiIn)
    got_trs, got_ws = {}, {}
    af_in.set_callback(lambda ev, _: got_trs.setdefault(tuple(ev[0]), time.perf_counter()))
    ok = True
    async with websockets.connect(f"ws://{host}/ws") as ws:
        await asyncio.sleep(0.5)
        sent = {}
        for i in range(100):  # page -> TRS: CC 74 on channel 1, values 0..99
            m = (0xB0, 74, i)
            sent[m] = time.perf_counter()
            await ws.send(struct.pack(">I", 0x20000000 | m[0] << 16 | m[1] << 8 | m[2]))
            await asyncio.sleep(0.02)
        await asyncio.sleep(0.5)
        lat = [(got_trs[m] - sent[m]) * 1000 for m in sent if m in got_trs]
        ok &= len(lat) == 100
        print(f"  {'PASS' if len(lat) == 100 else 'FAIL'}  page -> TRS: {len(lat)}/100, median "
              f"{statistics.median(lat):.1f} ms, max {max(lat):.1f}" if lat else "  FAIL  page -> TRS: nothing")

        sent2 = {}

        async def reader():
            while True:
                data = await ws.recv()
                t = time.perf_counter()
                for k in range(0, len(data) - 3, 4):
                    w = struct.unpack(">I", data[k:k + 4])[0]
                    if w >> 28 == 2:
                        got_ws.setdefault(((w >> 16) & 0xFF, (w >> 8) & 0x7F, w & 0x7F), t)

        task = asyncio.create_task(reader())
        for i in range(100):  # TRS -> page
            m = (0xB2, 10, i)
            sent2[m] = time.perf_counter()
            af_out.send_message(list(m))
            await asyncio.sleep(0.02)
        await asyncio.sleep(0.5)
        task.cancel()
        lat = [(got_ws[m] - sent2[m]) * 1000 for m in sent2 if m in got_ws]
        ok &= len(lat) == 100
        print(f"  {'PASS' if len(lat) == 100 else 'FAIL'}  TRS -> page: {len(lat)}/100, median "
              f"{statistics.median(lat):.1f} ms, max {max(lat):.1f}" if lat else "  FAIL  TRS -> page: nothing")
    print("ALL PASS" if ok else "FAILED")
    sys.exit(0 if ok else 1)


asyncio.run(main())

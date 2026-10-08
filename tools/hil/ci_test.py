"""MIDI-CI end to end: the Mac stand-in (devices/mac_standin) plays the device on the AudioFuse ports, cabled to
the box's TRS jacks. The box (Network mode) scans it, the web API serves exactly what the stand-in holds, a cached
rescan skips Property Exchange, a control sent from the page arrives at the device as a Control Change, another
MIDI-CI host behind the box (here on the WebSocket) can talk to the device through it, profiles can be stored and
removed, and a scan with nobody on the jacks ends in "no device".

    devices/mac_standin/build.sh
    uv run --with websockets python tools/hil/ci_test.py <host>
"""

import asyncio
import json
import pathlib
import queue
import random
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

import websockets

host = sys.argv[1]
standin = pathlib.Path(__file__).resolve().parents[2] / "devices/mac_standin/build/mac_standin"
results = []


def call(method, path, body=None, raw_body=None):
    data = raw_body if raw_body is not None else json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"http://{host}{path}", data=data, method=method,
                                 headers={"content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}")


def scan(force):
    """Start a scan and wait for its result (the status changes from scanning back to idle)."""
    call("POST", "/api/ci/scan", {"force": force})
    end = time.time() + 10
    seen_scanning = False
    while time.time() < end:
        _, s = call("GET", "/api/ci")
        seen_scanning |= s["scanning"]
        if seen_scanning and not s["scanning"]:
            return s
        time.sleep(0.1)
    return s


class Standin:
    def __init__(self):
        self.p = subprocess.Popen([str(standin), "--seconds", "120"], stdout=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        first = self.lines.get(timeout=5)
        assert first.startswith("ready"), first

    def _read(self):
        for line in self.p.stdout:
            self.lines.put(line.strip())

    def wait_for(self, prefix, timeout=2):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            if line.startswith(prefix):
                return line
        return None

    def drain(self):
        while not self.lines.empty():
            self.lines.get()

    def stop(self):
        self.p.terminate()
        self.p.wait()


expected = json.loads(subprocess.run([str(standin), "--dump"], capture_output=True, text=True).stdout)
dev = Standin()

s = scan(force=True)
check("scan finds the stand-in", s["result"] == "ok" and s["device"] == "7d0000-0000-0002-00010000",
      f"{s['result']} {s['device']} in {s['scan_ms']} ms")
check("scan under 2 s at 31250 baud", s["scan_ms"] < 2000, f"{s['scan_ms']} ms")
code, d = call("GET", "/api/ci/device")
check("DeviceInfo as served by the device", d["info"] == expected["info"])
check("AllCtrlList as served by the device", d["controls"] == expected["controls"], f"{len(d['controls'])} controls")
check("CtrlMapList for each ctrlMapId", d["maps"] == expected["maps"], list(d["maps"]))

dev.drain()
s = scan(force=False)
check("rescan of a known device uses the cache", s["result"] == "cached" and s["source"] == "cache",
      f"{s['result']} in {s['scan_ms']} ms")
gets = []
while (line := dev.wait_for("ci", 0.3)) is not None:
    gets.append(line)
check("cached rescan asks no Property Exchange", not any("get reply" in g for g in gets), gets)
code, d = call("GET", "/api/ci/device")
check("cached control list is the same", d["controls"] == expected["controls"])


async def page_to_device():
    """A fader on the page (MIDI 2.0 CC, 32 bits) arrives at the device as a MIDI 1.0 CC."""
    got = []
    async with websockets.connect(f"ws://{host}/ws") as ws:
        await asyncio.sleep(0.3)
        dev.drain()
        for value, want in [(0x00000000, 0), (0x80000000, 64), (0xFFFFFFFF, 127)]:
            await ws.send(struct.pack(">II", 0x40B04A00, value))  # MT 4, group 0, CC on channel 1, index 74
            got.append((dev.wait_for("cc", 2), f"cc 1 74 {want}"))
    return got


for line, want in asyncio.run(page_to_device()):
    check(f"page -> device: {want}", line == want, line)


def m7(v, n=4):
    return [(v >> (7 * i)) & 0x7F for i in range(n)]


def sysex7_words(sx):
    """SysEx contents -> UMP SysEx7 packets (two words each)."""
    chunks = [sx[i:i + 6] for i in range(0, len(sx), 6)] or [[]]
    words = []
    for k, c in enumerate(chunks):
        st = 0 if len(chunks) == 1 else 1 if k == 0 else 3 if k == len(chunks) - 1 else 2
        b = list(c) + [0] * (6 - len(c))
        words += [0x30000000 | st << 20 | len(c) << 16 | b[0] << 8 | b[1], b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]]
    return struct.pack(f">{len(words)}I", *words)


async def host_behind_the_box():
    """A MIDI-CI initiator on the WebSocket (as macOS would be on BLE or the network): Discovery, then Get
    AllCtrlList, both answered by the device on TRS through the box. Returns the reassembled control list."""
    me = random.randrange(0x0FFFFF00)
    msgs, cur = [], []

    async def recv_until(pred, timeout=5):
        nonlocal cur
        end = time.time() + timeout
        while time.time() < end:
            try:
                data = await asyncio.wait_for(ws.recv(), max(0.05, end - time.time()))
            except asyncio.TimeoutError:
                break
            w = struct.unpack(f">{len(data) // 4}I", data)
            i = 0
            while i < len(w):
                mt = w[i] >> 28
                n = [1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4][mt]
                if mt == 3 and i + 1 < len(w):
                    st, k = (w[i] >> 20) & 0xF, (w[i] >> 16) & 0xF
                    b = [(w[i] >> 8) & 0x7F, w[i] & 0x7F, w[i + 1] >> 24 & 0x7F, (w[i + 1] >> 16) & 0x7F,
                         (w[i + 1] >> 8) & 0x7F, w[i + 1] & 0x7F][:k]
                    if st in (0, 1):
                        cur = []
                    cur += b
                    if st in (0, 3):
                        msgs.append(cur)
                i += n
            if pred():
                return True
        return pred()

    def to_me(sub2):
        return [m for m in msgs if len(m) > 13 and m[3] == sub2 and sum(m[9 + i] << 7 * i for i in range(4)) == me]

    async with websockets.connect(f"ws://{host}/ws") as ws:
        await asyncio.sleep(0.3)
        await ws.send(sysex7_words([0x7E, 0x7F, 0x0D, 0x70, 2] + m7(me) + [0x7F] * 4 +
                                   [0x7D, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 8] + m7(512) + [0]))
        if not await recv_until(lambda: to_me(0x71)):
            return None
        reply = to_me(0x71)[0]
        dev_muid = sum(reply[5 + i] << 7 * i for i in range(4))
        hdr = list(b'{"resource":"AllCtrlList"}')
        await ws.send(sysex7_words([0x7E, 0x7F, 0x0D, 0x34, 2] + m7(me) + m7(dev_muid) + [5] + m7(len(hdr), 2) +
                                   hdr + [1, 0, 1, 0, 0, 0]))

        def chunks():
            out = {}
            for m in to_me(0x35):
                d = m[13:]
                hl = d[1] | d[2] << 7
                p = d[3 + hl:]
                total, this, n = p[0] | p[1] << 7, p[2] | p[3] << 7, p[4] | p[5] << 7
                out[this] = (total, bytes(p[6:6 + n]))
            return out

        await recv_until(lambda: (c := chunks()) and len(c) == next(iter(c.values()))[0], 10)
        c = chunks()
        return b"".join(c[k][1] for k in sorted(c)) if c else None


body = asyncio.run(host_behind_the_box())
check("host behind the box reads AllCtrlList through it", body is not None and json.loads(body) == expected["controls"],
      f"{len(body)} bytes" if body else "no reply")
_, s = call("GET", "/api/ci")
check("the box's own initiator stays out of it", not s["scanning"] and s["result"] == "cached", s["result"])

profile = [{"title": "Volume", "channel": 2, "ctrlType": "cc", "ctrlIndex": [7], "default": 0}]
code, r = call("POST", "/api/ci/profile", raw_body=json.dumps(profile).encode())
check("profile stored", code == 200, r)
code, d = call("GET", "/api/ci/device")
check("profile served next to the device's list", d["profile"] == profile and d["controls"] == expected["controls"])
code, r = call("POST", "/api/ci/profile", raw_body=b'{"no": "controls"}')
check("profile without controls rejected", code == 400, r.get("error"))
code, r = call("POST", "/api/ci/profile", raw_body=b"[{")
check("broken JSON rejected", code == 400, r.get("error"))

dev.stop()
s = scan(force=False)
check("nobody on the jacks: no device", s["result"] == "no device" and s["scan_ms"] >= 3000, f"{s['scan_ms']} ms")
check("then the profile is the control list", s["source"] == "profile", s["source"])
code, r = call("DELETE", "/api/ci/profile")
_, s = call("GET", "/api/ci")
check("profile removed", code == 200 and s["source"] == "", s["source"])
check("no MIDI-CI message lost between bus and CI task", s["dropped"] == 0, s["dropped"])

print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

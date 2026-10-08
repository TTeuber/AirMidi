"""MIDI-CI end to end: the Mac stand-in (devices/mac_standin) plays the device on the AudioFuse ports, cabled to
the box's TRS jacks. The box (Network mode) scans it, the web API serves exactly what the stand-in holds, a cached
rescan skips Property Exchange, a control sent from the page arrives at the device as a Control Change, profiles
can be stored and removed, and a scan with nobody on the jacks ends in "no device".

    devices/mac_standin/build.sh
    uv run --with websockets python tools/hil/ci_test.py <host>
"""

import asyncio
import json
import pathlib
import queue
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

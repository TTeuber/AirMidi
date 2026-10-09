"""The ESP-NOW cable between two boxes, timed one way on the Mac's clock:

    to the S3:  Mac -> AudioFuse -> J3 IN -> C6 box -> ESP-NOW -> S3 box -> USB MIDI -> Mac
    to the C6:  Mac -> USB MIDI -> S3 box -> ESP-NOW -> C6 box -> J2 OUT -> AudioFuse -> Mac

The C6 is in Network mode (on the access point's channel); the S3 in Play mode, so it has to find the C6's channel
by itself (lib/espnow_link). Both get ESP-NOW switched on (the C6 through the web API, the S3 through its console)
and off again at the end. Every message must arrive, in order and unchanged: 300 CCs each way at 20 ms and 600 at
5 ms, and a 300-byte SysEx.

    uv run --with python-rtmidi --with pyserial python tools/hil/espnow_test.py <C6 host> <S3 USB MIDI name>
"""

import glob
import json
import statistics
import sys
import time
import urllib.request

import rtmidi
import serial

host, s3 = sys.argv[1], sys.argv[2]
results = []


def check(what, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {what}{': ' + str(detail) if detail else ''}", flush=True)


def get(path):
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=3) as r:
            return json.load(r)
    except Exception:
        return None


def post(path, obj):
    req = urllib.request.Request(f"http://{host}{path}", data=json.dumps(obj).encode(), method="POST",
                                 headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=3) as r:
        return json.load(r)


def s3_console(cmd, wait=1.0):
    """A command on the S3's console (TinyUSB's serial port, named after its MAC); returns what it printed."""
    port = next(p for p in glob.glob("/dev/cu.usbmodem*") if len(p.split("usbmodem")[1]) >= 12)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = True  # both: never esptool's reset pattern
    s.open()
    s.read(65536)
    s.write(cmd)
    time.sleep(wait)
    try:
        out = s.read(65536).decode(errors="replace")
        s.close()
    except (serial.SerialException, OSError):
        out = ""  # it restarted (E): the port went away
    return out


def wait_for(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.5)
    return False


def port(cls, part):
    p = cls()
    p.open_port(next(i for i, n in enumerate(p.get_ports()) if part in n))
    if cls is rtmidi.MidiIn:
        p.ignore_types(sysex=False, timing=False, active_sense=True)
    return p


def run(src, dst, label, n, gap):
    out, inp = port(rtmidi.MidiOut, src), port(rtmidi.MidiIn, dst)
    got = []
    inp.set_callback(lambda ev, _: got.append((time.time(), list(ev[0]))))
    sent = []
    for i in range(n):
        m = [0xB0 | (i % 16), 1 + i // 16 % 100, i % 128]
        sent.append((time.time(), m))
        out.send_message(m)
        time.sleep(gap)
    sx = [0xF0, 0x7D] + [i % 128 for i in range(300)] + [0xF7]
    out.send_message(sx)
    time.sleep(1.5)
    out.close_port(), inp.close_port()
    have = [m for _, m in got if m[0] != 0xF0]
    want = [m for _, m in sent]
    lat = sorted((t2 - t1) * 1000 for (t1, m1), (t2, m2) in zip(sent, [g for g in got if g[1][0] != 0xF0]) if m1 == m2)
    check(f"{label}, {n} at {gap * 1000:.0f} ms: all, in order", have == want,
          f"{len(have)}/{n}, median {statistics.median(lat):.1f} ms, p95 {lat[int(0.95 * (len(lat) - 1))]:.1f}"
          if lat else f"{len(have)}/{n}")
    check(f"{label}: 300-byte SysEx whole", sx in [m for _, m in got])


before = get("/api/settings")
if not before:
    sys.exit(f"no answer from http://{host}")
if not before["espnow"]:
    post("/api/settings", {**before, "espnow": True})  # restarts
    time.sleep(3)
if "espnow:" not in s3_console(b"s"):
    s3_console(b"E")  # restarts
    time.sleep(8)
t0 = time.time()
found = wait_for(lambda: s3 in ((get("/api/status") or {}).get("espnow") or {}).get("peers", []), 60)
st = (get("/api/status") or {}).get("espnow", {})
check("the boxes find each other", found, f"{time.time() - t0:.1f} s, C6 on channel {st.get('channel')}")
print("        S3:", next((l for l in s3_console(b"s").splitlines() if l.startswith("espnow")), "?"), flush=True)

run("Audiofuse", s3, "TRS -> C6 -> ESP-NOW -> S3 -> USB", 300, 0.02)
run(s3, "Audiofuse", "USB -> S3 -> ESP-NOW -> C6 -> TRS", 300, 0.02)
run("Audiofuse", s3, "TRS -> C6 -> ESP-NOW -> S3 -> USB", 600, 0.005)
run(s3, "Audiofuse", "USB -> S3 -> ESP-NOW -> C6 -> TRS", 600, 0.005)
st = (get("/api/status") or {}).get("espnow", {})
print(f"        C6: rx {st.get('rx')} frames, {st.get('lost')} lost, {st.get('dups')} duplicates dropped")

post("/api/settings", before)  # ESP-NOW back as it was on the C6
s3_console(b"E")  # and off on the S3
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

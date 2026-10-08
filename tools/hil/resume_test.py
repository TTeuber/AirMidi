"""Network MIDI 2.0 session resumption: macOS never invites a host again after a session ends, so the box takes
the session back after it restarts (lib/nm2, "Resuming"). Connect once, then reset the box several times; after
each reset MIDI must flow both ways again with nobody touching Audio MIDI Setup, and macOS must still list the box.

    swiftc -O tools/hil/nm2connect.swift -o tools/hil/build/nm2connect
    uv run --with python-rtmidi --with pyserial python tools/hil/resume_test.py <host> [resets]
"""

import glob
import json
import subprocess
import sys
import time
import urllib.request

import rtmidi
import serial

host = sys.argv[1]
resets = int(sys.argv[2]) if len(sys.argv) > 2 else 3
nm2connect = "tools/hil/build/nm2connect"
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}", flush=True)


def session():
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=2) as r:
            return json.load(r)["net"][0]
    except Exception:
        return None


def mac_lists_box():
    out = subprocess.run([nm2connect, "--show"], capture_output=True, text=True).stdout
    return "airmidi" in out and "running" in out


def port(cls, part):
    p = cls()
    p.open_port(next(i for i, n in enumerate(p.get_ports()) if part in n))
    return p


def flow(src_part, dst_part, n=20):
    """Send n CCs into src, count how many arrive at dst."""
    out, inp = port(rtmidi.MidiOut, src_part), port(rtmidi.MidiIn, dst_part)
    got = set()
    inp.set_callback(lambda ev, _: got.add(tuple(ev[0])))
    sent = [(0xB3, 20, i) for i in range(n)]
    for m in sent:
        out.send_message(list(m))
        time.sleep(0.02)
    time.sleep(0.5)
    out.close_port(), inp.close_port()
    return sum(m in got for m in sent)


def reset_box():
    s = serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200)
    s.dtr = False  # DTR high would select download mode
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.close()


SESSION, TRS = "Network MIDI 2.0 Session", "Audiofuse"

# A fresh invitation (the box remembers the Mac): drop any session first, or macOS sees nothing to do.
subprocess.run([nm2connect, "--remove", "airmidi-7530"], capture_output=True)
time.sleep(2)
subprocess.run([nm2connect, "airmidi-7530"], capture_output=True)
end = time.time() + 10
while time.time() < end and not (session() or {}).get("active"):
    time.sleep(0.5)
time.sleep(1)
check("connected", (session() or {}).get("active", False))
check("Mac -> TRS before any reset", flow(SESSION, TRS) == 20)

for k in range(1, resets + 1):
    reset_box()
    t0 = time.time()
    time.sleep(3)
    s = None
    while time.time() - t0 < 30:
        s = session()
        if s and s["active"]:
            break
        time.sleep(0.25)
    back = time.time() - t0
    check(f"reset {k}: session back without a reconnect", bool(s and s["active"]), f"{back:.1f} s after the reset")
    # box -> Mac first: this direction only works if the box took the session back on its own
    check(f"reset {k}: TRS -> Mac", (n := flow(TRS, SESSION)) == 20, f"{n}/20")
    check(f"reset {k}: Mac -> TRS", (n := flow(SESSION, TRS)) == 20, f"{n}/20")
    check(f"reset {k}: macOS still lists the box", mac_lists_box())

print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

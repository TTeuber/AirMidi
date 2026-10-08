"""macOS finds the MIDI-CI device behind the box over Network MIDI 2.0: the box announces its endpoint when a session
starts, macOS then asks for the function block and runs MIDI-CI Discovery through the box to the device on TRS (the
Mac stand-in on the AudioFuse), and its MIDICIDeviceManager lists that device. macOS also passes the box's own
Discovery (which reaches it through the AudioFuse) back to the box over the network: the box must drop that copy
(lib/ci_guard), or it circles until the TRS line is full. Then the box is reset: the resumed session announces
again, and MIDI still flows both ways (macOS switches the session to MIDI 2.0 once told).

    devices/mac_standin/build.sh
    swiftc -O tools/hil/nm2connect.swift -o tools/hil/build/nm2connect
    swiftc -O tools/hil/cidevices.swift -o tools/hil/build/cidevices
    uv run --with python-rtmidi --with pyserial python tools/hil/mac_ci_test.py <host>
"""

import glob
import json
import pathlib
import queue
import subprocess
import sys
import threading
import time
import urllib.request

import rtmidi
import serial

host = sys.argv[1]
root = pathlib.Path(__file__).resolve().parents[2]
standin = root / "devices/mac_standin/build/mac_standin"
nm2connect, cidevices = root / "tools/hil/build/nm2connect", root / "tools/hil/build/cidevices"
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}", flush=True)


def get(path):
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=2) as r:
            return json.load(r)
    except Exception:
        return None


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


def wait_session(timeout):
    end = time.time() + timeout
    while time.time() < end:
        if ((get("/api/status") or {}).get("net") or [{}])[0].get("active"):
            return True
        time.sleep(0.25)
    return False


def discovered():
    """MUIDs (hex) macOS's MIDICIDeviceManager lists, with their Property Exchange flag."""
    out = subprocess.run([str(cidevices), "", "1"], capture_output=True, text=True).stdout
    return {line.split()[2].rstrip(","): "PE true" in line for line in out.splitlines()
            if line.startswith("discovered:")}


p = subprocess.Popen([str(standin), "--seconds", "120"], stdout=subprocess.PIPE, text=True)
lines = queue.Queue()
threading.Thread(target=lambda: [lines.put(line.strip()) for line in p.stdout], daemon=True).start()
dev_muid = lines.get(timeout=5).split()[1]


def mac_discoveries(timeout):
    """Discovery replies the device sent to someone other than the box, within timeout."""
    box_muid = (get("/api/ci") or {}).get("muid", "")
    got, end = [], time.time() + timeout
    while time.time() < end:
        try:
            line = lines.get(timeout=0.1)
        except queue.Empty:
            continue
        if line.startswith("ci discovery reply to") and line.split()[4] != box_muid:
            got.append(line.split()[4])
    return got


subprocess.run([str(nm2connect), "--remove", "airmidi-7530"], capture_output=True)
time.sleep(2)
subprocess.run([str(nm2connect), "airmidi-7530"], capture_output=True)
check("connected", wait_session(10))
asked = mac_discoveries(5)
check("macOS runs MIDI-CI Discovery through the box", bool(asked), f"from MUID {asked[0]}" if asked else "none")
found = discovered()
check("MIDICIDeviceManager lists the device on TRS, with Property Exchange", found.get(dev_muid) is True,
      f"device {dev_muid}, listed {sorted(found)}")
check("Mac -> TRS", (n := flow("Network MIDI 2.0 Session", "Audiofuse")) == 20, f"{n}/20")
check("TRS -> Mac", (n := flow("Audiofuse", "Network MIDI 2.0 Session")) == 20, f"{n}/20")



def net_in():
    return next(p["in"] for p in get("/api/status")["ports"] if p["name"] == "net1")


before = net_in()
urllib.request.urlopen(urllib.request.Request(f"http://{host}/api/ci/scan", data=b'{"force": true}', method="POST"))
time.sleep(4)
scan, came_back = get("/api/ci"), net_in() - before
check("box scan with macOS on the network", scan["result"] == "ok", f"{scan['result']} in {scan['scan_ms']} ms")
check("its Discovery does not loop through the Mac", came_back < 200, f"{came_back} messages from macOS in 4 s")

reset_box()
time.sleep(3)
check("reset: session resumed", wait_session(30))
asked = mac_discoveries(5)
check("reset: macOS asks again", bool(asked), f"from MUID {asked[0]}" if asked else "none")
check("reset: still listed", discovered().get(dev_muid) is True)
check("reset: TRS -> Mac", (n := flow("Audiofuse", "Network MIDI 2.0 Session")) == 20, f"{n}/20")
check("reset: Mac -> TRS", (n := flow("Network MIDI 2.0 Session", "Audiofuse")) == 20, f"{n}/20")

p.terminate()
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

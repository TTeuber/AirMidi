"""RTP-MIDI with macOS's own driver: one of the Mac's RTP sessions (made in Audio MIDI Setup's MIDI Network window)
connects to the box; MIDI flows both ways through the AudioFuse with nothing lost (latency.py: 300 messages at 20 ms
and 600 at 5 ms each way); then the box is reset, and the Mac's next clock sync must take the session back (lib/rtpmidi,
"Surviving a restart") with no one touching Audio MIDI Setup.

    swiftc -O tools/hil/rtpconnect.swift -o tools/hil/build/rtpconnect
    uv run --with python-rtmidi --with pyserial python tools/hil/rtp_test.py <host> "<session>" [resets]
"""

import glob
import json
import re
import subprocess
import sys
import time
import urllib.request

import rtmidi
import serial

host, session_name = sys.argv[1], sys.argv[2]
resets = int(sys.argv[3]) if len(sys.argv) > 3 else 2
rtpconnect = "tools/hil/build/rtpconnect"
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}", flush=True)


def rtp():
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=2) as r:
            return json.load(r)["rtp"][0]
    except Exception:
        return None


def wait_for(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return False


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


def latency(count, gap, offset):
    out = subprocess.run([sys.executable, "tools/hil/latency.py", session_name, str(count), str(gap), str(offset)],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "->" in line:
            m = re.search(r"median\s+([\d.]+).*p95\s+([\d.]+).*lost (\d+)/(\d+)", line)
            direction = "Mac -> TRS" if line.strip().startswith("wireless") else "TRS -> Mac"
            # Reordering is expected towards the box: a lost packet's commands are repaired from the next one's
            # journal, after the commands that came in between.
            ok = m and m.group(3) == "0"
            check(f"{direction}, {count} at {gap} ms", bool(ok),
                  (f"median {m.group(1)} ms, p95 {m.group(2)} ms, lost {m.group(3)}/{m.group(4)}"
                   + (", repairs out of order" if "REORDERED" in line else "")) if m else line)


def reset_box():
    s = serial.Serial(glob.glob("/dev/cu.usbmodem1101")[0], 115200)
    s.dtr = False  # DTR high would select download mode
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.close()


# macOS ends a session no CoreMIDI client has open, so this one stays open throughout.
listener = port(rtmidi.MidiIn, session_name)
subprocess.run([rtpconnect, session_name, "airmidi-7530"], capture_output=True)
check("connected", wait_for(lambda: (rtp() or {}).get("active"), 15), (rtp() or {}).get("peer"))
time.sleep(6)  # macOS drops MIDI from a peer until its first clock syncs are done (~5 s)
latency(300, 20, 0)
latency(600, 5, 64)  # other values than the first run: a lost CC must be repaired, not already right
s = rtp() or {}
print(f"        box: received {s.get('rx')} packets, lost {s.get('lost')}, late {s.get('late')}, "
      f"repaired {s.get('repairs')} commands, unreadable {s.get('bad')}")

for r in range(resets):
    reset_box()
    t0 = time.time()
    back = wait_for(lambda: (s := rtp()) and s.get("active") and s.get("resumed"), 30)
    check(f"reset {r + 1}: session taken back", back, f"{time.time() - t0:.1f} s, peer {(rtp() or {}).get('peer')}")
    t1 = time.time()
    synced = wait_for(lambda: (s := rtp()) and not s.get("syncing"), 6)
    check(f"reset {r + 1}: sequence numbers settled", synced, f"{time.time() - t1:.1f} s later")
    check(f"reset {r + 1}: Mac -> TRS", (n := flow(session_name, "Audiofuse")) == 20, f"{n}/20")
    check(f"reset {r + 1}: TRS -> Mac", (n := flow("Audiofuse", session_name)) == 20, f"{n}/20")

subprocess.run([rtpconnect, "--remove", session_name], capture_output=True)
listener.close_port()
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

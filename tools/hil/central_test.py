"""The box as a BLE central: it finds a BLE MIDI device by itself, connects, and bridges it to TRS. The device is
the Mac (tools/hil/blepeer, a CoreBluetooth peripheral standing in for a BLE MIDI keyboard), and the other end is
the AudioFuse, so each message is timed one way on the Mac's clock:

    device -> box:  blepeer notifies -> box (central) -> J2 OUT -> AudioFuse MIDI IN -> Mac
    box -> device:  Mac -> AudioFuse MIDI OUT -> J3 IN -> box -> write to blepeer -> Mac

macOS's peripheral advertises no name, so the box is told to connect to any BLE MIDI device ("*"). Checked: it
connects; MIDI flows both ways, every message, with its latency; it lets go and connects again when the setting
changes and after a reset; and the same in Play mode (Wi-Fi off, BLE has the radio to itself). The settings are put
back after.

    swiftc -O tools/hil/blepeer.swift -o tools/hil/build/blepeer
    uv run --with python-rtmidi --with pyserial python tools/hil/central_test.py <host>
"""

import glob
import json
import pathlib
import queue
import statistics
import subprocess
import sys
import threading
import time
import urllib.request

import rtmidi
import serial

host = sys.argv[1]
root = pathlib.Path(__file__).resolve().parents[2]
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


def post(path, obj):
    req = urllib.request.Request(f"http://{host}{path}", data=json.dumps(obj).encode(), method="POST",
                                 headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=3) as r:
        return json.load(r)


def wait_for(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return False


def device_state():
    return ((get("/api/status") or {}).get("device") or {}).get("state")


class Peer:
    """tools/hil/blepeer: send() notifies a message; tx/rx lines carry the Mac's clock."""

    def __init__(self):
        self.p = subprocess.Popen([str(root / "tools/hil/build/blepeer")], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lines = queue.Queue()
        self.tx, self.rx = {}, {}
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            parts = line.split()
            if parts[0] == "tx":
                self.tx.setdefault(parts[2], float(parts[1]))
            elif parts[0] == "rx":
                self.rx.setdefault(parts[2], float(parts[1]))
            else:
                self.lines.put(parts[0])

    def wait(self, what, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.lines.get(timeout=0.2) == what:
                    return True
            except queue.Empty:
                pass
        return False

    def send(self, msg):
        self.p.stdin.write("send " + msg.hex() + "\n")

    def stop(self):
        self.p.terminate()
        self.p.wait()


def port(cls, part):
    p = cls()
    p.open_port(next(i for i, n in enumerate(p.get_ports()) if part in n))
    return p


def stats(lat):
    lat = sorted(lat)
    return f"median {statistics.median(lat):.1f} ms, p95 {lat[int(0.95 * (len(lat) - 1))]:.1f}, max {lat[-1]:.1f}"


def device_to_box(peer, n=100, gap=0.02):
    """Notes from the device, timed from its notification to the AudioFuse input."""
    peer.tx.clear()  # every run sends the same messages: time them from this run's sends
    inp = port(rtmidi.MidiIn, "Audiofuse")
    got = {}
    inp.set_callback(lambda ev, _: got.setdefault(bytes(ev[0]).hex(), time.time()))
    sent = [bytes([0x95, 30 + i % 60, 1 + i]) for i in range(n)]  # unique: note and velocity
    for m in sent:
        peer.send(m)
        time.sleep(gap)
    time.sleep(1)
    inp.close_port()
    lat = [(got[m.hex()] - peer.tx[m.hex()]) * 1000 for m in sent if m.hex() in got and m.hex() in peer.tx]
    return lat, n


def box_to_device(peer, n=100, gap=0.02):
    """CCs into the AudioFuse, timed from the send to the device's write callback."""
    peer.rx.clear()
    out = port(rtmidi.MidiOut, "Audiofuse")
    sent = {}
    for i in range(n):
        m = bytes([0xB6, 20 + i % 50, i])
        sent[m.hex()] = time.time()
        out.send_message(list(m))
        time.sleep(gap)
    time.sleep(1)
    out.close_port()
    lat = [(peer.rx[h] - t) * 1000 for h, t in sent.items() if h in peer.rx]
    return lat, n


def both_ways(label, peer, n=100):
    lat, total = device_to_box(peer, n)
    check(f"{label}: device -> TRS", len(lat) == total, f"{len(lat)}/{total}, {stats(lat) if lat else ''}")
    lat, total = box_to_device(peer, n)
    check(f"{label}: TRS -> device", len(lat) == total, f"{len(lat)}/{total}, {stats(lat) if lat else ''}")


def reset_box():
    s = serial.Serial("/dev/cu.usbmodem1101", 115200)
    s.dtr = False  # DTR high would select download mode
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.close()


def console(cmd):
    """A console command over the USB serial port (in Play mode there is no web server)."""
    s = serial.Serial()
    s.port, s.baudrate = "/dev/cu.usbmodem1101", 115200
    s.dtr = s.rts = False  # set before opening: DTR high would select download mode, RTS resets
    s.open()
    time.sleep(0.5)
    s.write(cmd)
    s.flush()
    time.sleep(0.5)
    s.close()


before = get("/api/settings")
peer = Peer()
check("stand-in advertises", peer.wait("ready", 10))
r = post("/api/settings", {**before, "ble_in_network": True, "ble_device": "*"})
if r["restart"]:
    time.sleep(3)
check("box connects to the device", wait_for(lambda: device_state() == "connected", 40), device_state())
check("box subscribes to its notifications", peer.wait("subscribed", 10))
both_ways("network mode, BLE beside Wi-Fi", peer)

# The box lets go and connects again when its setting changes, and after a reset. Each time the stand-in starts
# afresh: macOS's peripheral does not advertise again after its first connection (a keyboard does, whenever it is
# not connected). (A device going away cannot be played either: quitting blepeer leaves macOS holding the link with
# the service gone, and CoreBluetooth has no call for a peripheral to drop a central.)
post("/api/settings", {**before, "ble_in_network": True, "ble_device": ""})
check("device setting off: box disconnects", peer.wait("unsubscribed", 10) or device_state() == "off", device_state())
peer.stop()
peer = Peer()
peer.wait("ready", 10)
post("/api/settings", {**before, "ble_in_network": True, "ble_device": "*"})
t0 = time.time()
ok = wait_for(lambda: device_state() == "connected", 30)
check("device setting on: box connects again", ok, f"{time.time() - t0:.1f} s")
peer.stop()
reset_box()
peer = Peer()
peer.wait("ready", 10)
t0 = time.time()
ok = wait_for(lambda: device_state() == "connected", 40)
check("box reset: connects again", ok, f"{time.time() - t0:.1f} s")
peer.wait("subscribed", 5)
both_ways("after reconnecting", peer, 20)

try:
    post("/api/mode", {"mode": "play"})  # BLE stays on: no restart, Wi-Fi goes off (maybe before the reply)
except OSError:
    pass
time.sleep(4)
both_ways("play mode, BLE alone", peer)
console(b"N")
check("back in network mode", wait_for(lambda: (get("/api/status") or {}).get("mode") == "network", 40))

post("/api/settings", {**before, "ble_in_network": True})
check("device setting cleared: box disconnects", peer.wait("unsubscribed", 10) or device_state() == "off")
post("/api/settings", before)  # BLE in network mode back as it was (restarts if it changes)
peer.stop()
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

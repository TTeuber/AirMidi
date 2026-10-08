"""Hardware-in-the-loop test of the TRS <-> BLE MIDI bridge, through the AudioFuse on the Mac.

    uv run --with python-rtmidi --with pyserial --with bleak python tools/hil/bridge_test.py [--bleak] [name]

name: the receiver's BLE name (default: the first CoreMIDI device starting with "airmidi-"). The script hands the
receiver to CoreMIDI first (tools/hil/blemidi connect; must happen before this process opens any CoreMIDI port).
If CoreMIDI will not take it (see docs/design/testing.md), or with --bleak, the BLE side goes straight through
CoreBluetooth instead (tools/hil/bleport.py): same checks, but latency then reflects macOS's 30 ms interval for
non-MIDI clients. Then it checks, in both directions:

  - notes, CCs on all 16 channels, clock + start/stop, SysEx of 253 and 1000 bytes (the long one spans many BLE
    packets), a burst of 300 notes with no gaps: every message arrives once, in order, unchanged;
  - both directions at once;
  - merge onto TRS: the receiver injects notes from a second port (test hook) while CoreMIDI sends a 1500-byte
    SysEx; the TRS output must carry the SysEx whole and every note;
  - one-way latency both ways (tools/hil/latency.py).

Exits non-zero if anything fails. The receiver's counters are printed at the end (console command 's').
"""

import glob
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).parent
BLEMIDI = HERE / "build" / "blemidi"


def connect_ble(name):
    if not BLEMIDI.exists():
        subprocess.run(["swiftc", "-O", str(HERE / "blemidi.swift"), "-o", str(BLEMIDI)], check=True)
    for attempt in range(4):  # activation can be ignored once after a disconnect (docs/design/testing.md)
        r = subprocess.run([str(BLEMIDI), "connect", name], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.strip()
        time.sleep(3)
    return None


def find_name():
    out = subprocess.run([str(BLEMIDI), "list"], capture_output=True, text=True).stdout
    names = [l.split(None, 1)[1] for l in out.splitlines() if l.split(None, 1)[1].startswith("airmidi-")]
    names = [n for n in names if n != "airmidi-s2"]
    if not names:
        sys.exit("no airmidi device known to CoreMIDI yet; pass its BLE name")
    return names[0]


args = [a for a in sys.argv[1:] if not a.startswith("--")]
use_bleak = "--bleak" in sys.argv
if not BLEMIDI.exists():
    subprocess.run(["swiftc", "-O", str(HERE / "blemidi.swift"), "-o", str(BLEMIDI)], check=True)
name = args[0] if args else find_name()
if not use_bleak:
    print(f"connecting {name} through CoreMIDI ...", flush=True)
    use_bleak = connect_ble(name) is None
    if use_bleak:
        print("CoreMIDI did not take it: using CoreBluetooth (bleak) directly", flush=True)
time.sleep(1.5)

import rtmidi  # noqa: E402  (only after the BLE device is online in CoreMIDI)

import latency  # noqa: E402


def open_pair(match):
    out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
    for p in (out, inp):
        idx = next((i for i, n in enumerate(p.get_ports()) if match.lower() in n.lower()), None)
        if idx is None:
            sys.exit(f"no port matching '{match}' in {p.get_ports()}")
        p.open_port(idx)
    inp.ignore_types(sysex=False, timing=False, active_sense=True)
    return out, inp


class Collector:
    def __init__(self, port):
        self.port, self.got = port, []
        port.set_callback(lambda ev, _: self.got.append(ev[0]))

    def take(self):
        g, self.got = self.got, []
        return g


def check(name, msgs, send, coll, gap, timeout=5.0, compare=None):
    coll.take()
    for m in msgs:
        send(m)
        if gap:
            time.sleep(gap)
    want = [list(m) for m in msgs]
    deadline = time.time() + timeout
    while len(coll.got) < len(want) and time.time() < deadline:
        time.sleep(0.02)
    time.sleep(0.2)
    got = coll.take()
    ok = (compare or (lambda g, w: g == w))(got, want)
    detail = f"{len(got)}/{len(want)} messages"
    if not ok and got != want:
        bad = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
        detail += f", first difference at #{bad}"
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}", flush=True)
    return ok


def suite(direction, send, coll):
    notes = [m for n in range(36, 96) for m in ([0x90, n, 100], [0x80, n, 0])]
    ccs = [[0xB0 | (i % 16), 74, i] for i in range(128)]
    clock = [[0xF8]] * 96 + [[0xFA], [0xFC]]
    sx253 = [[0xF0, 0x7D] + [i % 128 for i in range(251)] + [0xF7]]
    sx1000 = [[0xF0, 0x7D] + [(i * 7) % 128 for i in range(998)] + [0xF7]]
    burst = [[0x90 | (i % 16), i % 128, 1 + (i * 7) % 127] for i in range(300)]
    print(direction, flush=True)
    return [
        check("120 note on/off, 10 ms apart", notes, send, coll, 0.01),
        check("128 CCs on all 16 channels", ccs, send, coll, 0.005),
        check("96 clocks + start/stop", clock, send, coll, 0.005),
        check("SysEx, 253 bytes", sx253, send, coll, 0, timeout=3),
        check("SysEx, 1000 bytes", sx1000, send, coll, 0, timeout=5),
        check("burst of 300 notes, no gaps", burst, send, coll, 0, timeout=6),
    ]


def both_ways(ble_out, af_out, ble_coll, af_coll):
    """Both directions at once: 200 CCs each way, interleaved in time."""
    a = [[0xB0, 1, i % 128] for i in range(200)]
    b = [[0xB1, 2, i % 128] for i in range(200)]
    ble_coll.take(), af_coll.take()

    def sender(port, msgs):
        for m in msgs:
            port.send_message(m)
            time.sleep(0.004)

    t1 = threading.Thread(target=sender, args=(ble_out, a))
    t2 = threading.Thread(target=sender, args=(af_out, b))
    t1.start(), t2.start(), t1.join(), t2.join()
    time.sleep(1)
    ok1, ok2 = af_coll.take() == a, ble_coll.take() == b
    print(f"  {'PASS' if ok1 and ok2 else 'FAIL'}  both directions at once: BLE->TRS {'ok' if ok1 else 'BAD'}, "
          f"TRS->BLE {'ok' if ok2 else 'BAD'}", flush=True)
    return ok1 and ok2


def merge_test(ble_out, af_coll):
    """Two sources onto TRS: CoreMIDI sends a 1500-byte SysEx over BLE (~500 ms on the TRS wire) while the receiver
    injects 60 notes from another port (console command 'm', a test hook). The TRS output must carry the SysEx
    whole and every note, in order. (A second BLE central from the same Mac is not possible: it would share
    CoreMIDI's link and land in the same connection.)"""
    import serial

    sysex = [0xF0, 0x7D] + [(i * 3) % 128 for i in range(1500)] + [0xF7]
    notes = [[0x95, 40 + i % 40, 1 + i] for i in range(60)]
    af_coll.take()
    with serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200, timeout=0.3) as con:
        ble_out.send_message(sysex)
        time.sleep(0.05)
        con.write(b"m")
        time.sleep(2.5)
    got = af_coll.take()
    sx_ok = sysex in got
    notes_got = [m for m in got if m and m[0] == 0x95]
    ok = sx_ok and notes_got == notes and len(got) == len(notes) + 1
    print(f"  {'PASS' if ok else 'FAIL'}  merge onto TRS: SysEx {'whole' if sx_ok else 'BROKEN'}, "
          f"{len(notes_got)}/{len(notes)} injected notes{'' if notes_got == notes else ' (order/content BAD)'}",
          flush=True)
    return ok


def receiver_counters():
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        return "(no console)"
    import serial

    with serial.Serial(ports[0], 115200, timeout=0.3) as s:
        s.write(b"s")
        time.sleep(1)
        return s.read(4000).decode(errors="replace").strip()


def main():
    if use_bleak:
        from bleport import BlePort

        ble_out = ble_in = BlePort(name)
    else:
        ble_out, ble_in = open_pair(name)
    af_out, af_in = open_pair("Audiofuse")
    ble_coll, af_coll = Collector(ble_in), Collector(af_in)
    time.sleep(0.5)
    results = suite("BLE -> receiver -> TRS (AudioFuse in)", ble_out.send_message, af_coll)
    results += suite("TRS (AudioFuse out) -> receiver -> BLE", af_out.send_message, ble_coll)
    print("together", flush=True)
    results.append(both_ways(ble_out, af_out, ble_coll, af_coll))
    results.append(merge_test(ble_out, af_coll))
    for p in (ble_out, ble_in, af_out, af_in):
        p.close_port()

    print(f"latency ({'bleak' if use_bleak else 'CoreMIDI'}; the interval is in the counters below)", flush=True)
    if use_bleak:
        af_out, af_in = open_pair("Audiofuse")
        out = latency.run(ble_out, af_in, 300, 0.02)
        inn = latency.run(af_out, ble_in, 300, 0.02)
        ble_out.close()
    else:
        out, inn = latency.measure(name, 300, 20)
    print(latency.fmt("BLE -> TRS", out))
    print(latency.fmt("TRS -> BLE", inn))
    results.append(out["lost"] == 0 and inn["lost"] == 0)

    print("receiver counters:")
    for line in receiver_counters().splitlines():
        print("  |", line)
    print("ALL PASS" if all(results) else f"FAILED ({results.count(False)} of {len(results)})")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()

"""USB MIDI on an S3 box, with BLE MIDI as the other side (no TRS or Wi-Fi needed: a bare dev kit will do):

    to USB:   Mac -> BLE (bleak) -> box -> USB MIDI -> Mac
    from USB: Mac -> USB MIDI -> box -> BLE notification -> Mac

Every message must arrive, in order and unchanged: notes and CCs on all channels, clock, a long SysEx. Latency is
reported for information (BLE's interval dominates it, bleak keeps macOS at 15-30 ms).

    uv run --with python-rtmidi --with bleak python tools/hil/usb_test.py <ble name>      e.g. airmidi-6dac
"""

import statistics
import sys
import time
from pathlib import Path

import rtmidi

sys.path.insert(0, str(Path(__file__).parent))
from bleport import BlePort  # noqa: E402

name = sys.argv[1]
results = []


def check(what, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {what}{': ' + str(detail) if detail else ''}", flush=True)


def port(cls, part):
    p = cls()
    p.open_port(next(i for i, n in enumerate(p.get_ports()) if part in n))
    if cls is rtmidi.MidiIn:
        p.ignore_types(sysex=False, timing=False, active_sense=True)
    return p


def messages():
    out = []
    for i in range(160):
        ch = i % 16
        out.append([0x90 | ch, 36 + i % 60, 1 + i % 127] if i % 2 else [0xB0 | ch, i % 120, i % 128])
    out.insert(50, [0xF8])
    out.insert(100, [0xF0, 0x7D] + [i % 128 for i in range(300)] + [0xF7])
    return out


def run(send, recv_cb_setter, label):
    got = []
    recv_cb_setter(lambda ev, _: got.append((time.time(), list(ev[0]))))
    sent = []
    for m in messages():
        sent.append((time.time(), m))
        send(m)
        time.sleep(0.02 if m[0] != 0xF0 else 0.3)
    time.sleep(1.5)
    want = [m for _, m in sent]
    have = [m for _, m in got]
    check(f"{label}: every message, in order, unchanged", have == want, f"{len(have)}/{len(want)}")
    if have != want:
        extra = [m for m in have if m not in want]
        missing = [m for m in want if m not in have]
        print(f"        not sent: {extra[:6]} ({len(extra)}), missing: {[m[:6] for m in missing[:6]]} ({len(missing)})")
    lat = [(t2 - t1) * 1000 for (t1, m1), (t2, m2) in zip(sent, got) if m1 == m2 and m1[0] != 0xF0]
    if lat:
        print(f"        latency median {statistics.median(lat):.1f} ms, max {max(lat):.1f}")


ble = BlePort(name)
usb_in, usb_out = port(rtmidi.MidiIn, name), port(rtmidi.MidiOut, name)
run(ble.send_message, usb_in.set_callback, "BLE -> USB")
run(usb_out.send_message, ble.set_callback, "USB -> BLE")
ble.close()
usb_in.close_port(), usb_out.close_port()
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

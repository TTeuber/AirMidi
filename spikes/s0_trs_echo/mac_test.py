"""Spike S0, Mac side: send MIDI out of the AudioFuse, let the receiver echo it (J3 IN -> J2 OUT), read it back on
the AudioFuse's MIDI in, and check that every message returns unchanged and in order.

    uv run --with python-rtmidi --with pyserial python spikes/s0_trs_echo/mac_test.py

The receiver's console (USB) is read at the same time: if nothing comes back, it tells which cable is at fault
(bytes reached the receiver = the AudioFuse -> receiver direction works).
"""

import glob
import statistics
import sys
import threading
import time

import rtmidi
import serial

PORT_NAME = "Audiofuse"
console_lines = []


def read_console():
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        return
    with serial.Serial(ports[0], 115200, timeout=0.2) as s:
        while True:
            line = s.readline().decode(errors="replace").strip()
            if line:
                console_lines.append(line)


def open_port(cls):
    port = cls()
    names = port.get_ports()
    idx = next((i for i, n in enumerate(names) if PORT_NAME.lower() in n.lower()), None)
    if idx is None:
        sys.exit(f"no '{PORT_NAME}' port in {names}")
    port.open_port(idx)
    return port


def run(name, msgs, gap_s, midi_in, midi_out, timeout_s=2.0):
    """Send msgs with gap_s between them; return (ok, round-trip times in ms)."""
    received = []
    midi_in.set_callback(lambda ev, _: received.append((time.perf_counter(), ev[0])))
    sent_at = []
    for m in msgs:
        sent_at.append(time.perf_counter())
        midi_out.send_message(m)
        if gap_s:
            time.sleep(gap_s)
    deadline = time.perf_counter() + timeout_s
    while len(received) < len(msgs) and time.perf_counter() < deadline:
        time.sleep(0.01)
    midi_in.cancel_callback()
    got = [m for _, m in received]
    ok = got == [list(m) for m in msgs]
    rtt = [(t - s) * 1000 for (t, _), s in zip(received, sent_at)] if ok else []
    detail = f"{len(got)}/{len(msgs)} back"
    if not ok:
        first_bad = next((i for i, (a, b) in enumerate(zip(got, msgs)) if a != list(b)), min(len(got), len(msgs)))
        detail += f", first mismatch at #{first_bad}"
    elif gap_s and rtt:
        detail += f", round trip median {statistics.median(rtt):.2f} ms, min {min(rtt):.2f}, max {max(rtt):.2f}"
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
    return ok


def main():
    threading.Thread(target=read_console, daemon=True).start()
    midi_out, midi_in = open_port(rtmidi.MidiOut), open_port(rtmidi.MidiIn)
    midi_in.ignore_types(sysex=False, timing=False, active_sense=True)
    time.sleep(0.3)

    notes = [m for n in range(36, 96) for m in ([0x90, n, 100], [0x80, n, 0])]
    ccs = [[0xB0 | (i % 16), 74, i] for i in range(128)]
    clock = [[0xF8]] * 96 + [[0xFA], [0xFC]]
    sysex = [[0xF0, 0x7D] + [i % 128 for i in range(250)] + [0xF7]]
    burst = [[0x90 | (i % 16), i % 128, (i * 7) % 127 + 1] for i in range(300)]

    print("S0: AudioFuse MIDI OUT -> receiver J3 IN -> echo -> J2 OUT -> AudioFuse MIDI IN")
    results = [
        run("120 note on/off, 20 ms apart", notes, 0.02, midi_in, midi_out),
        run("128 CCs on all 16 channels", ccs, 0.005, midi_in, midi_out),
        run("96 clocks + start/stop", clock, 0.005, midi_in, midi_out),
        run("SysEx, 253 bytes", sysex, 0, midi_in, midi_out),
        run("burst of 300 notes, no gaps", burst, 0, midi_in, midi_out, timeout_s=4),
    ]
    time.sleep(1.2)
    print("receiver console:")
    for line in console_lines[-8:]:
        print("  |", line)
    if not all(results):
        reached = any(l.startswith(("rx:", "echoed")) for l in console_lines)
        print("diagnosis:", "AudioFuse -> receiver works; check receiver J2 OUT -> AudioFuse IN cable"
              if reached else "nothing reached the receiver: check the AudioFuse OUT -> receiver J3 IN cable")
    print("ALL PASS" if all(results) else "FAILED")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()

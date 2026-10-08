"""Does MIDI survive the power modes? For each mode (console '0'-'2', see firmware power.c): 15 single messages
each way with 1.5 s of silence before each (time for the chip to fall asleep), then a 200-message stream at 20 ms.
Counts what arrives and how late, through the AudioFuse, over bleak (tools/hil/bleport.py).

    uv run --with python-rtmidi --with pyserial --with bleak python tools/hil/sleep_test.py [name] [modes]

modes: e.g. 012 (default). With 'wait' after the modes, it pauses in each mode for a power meter reading.
"""

import glob
import statistics
import sys
import time

import rtmidi
import serial

from bleport import BlePort

name = sys.argv[1] if len(sys.argv) > 1 else "airmidi-7530"
modes = sys.argv[2] if len(sys.argv) > 2 else "012"
wait = "wait" in sys.argv


def af_ports():
    o, i = rtmidi.MidiOut(), rtmidi.MidiIn()
    for p in (o, i):
        p.open_port(next(n for n, s in enumerate(p.get_ports()) if "Audiofuse" in s))
    i.ignore_types(sysex=False, timing=False, active_sense=True)
    return o, i


def run(send, recv, msgs, gap):
    got = {}
    recv.set_callback(lambda ev, _: got.setdefault(tuple(ev[0]), time.perf_counter()))
    sent = {}
    for m in msgs:
        if gap > 1:
            time.sleep(gap)
        sent[tuple(m)] = time.perf_counter()
        send.send_message(m)
        if gap <= 1:
            time.sleep(gap)
    time.sleep(1.0)
    recv.cancel_callback()
    lat = [(got[k] - sent[k]) * 1000 for k in sent if k in got]
    med = f"median {statistics.median(lat):6.1f} ms, max {max(lat):6.1f}" if lat else "nothing"
    return f"{len(lat):3d}/{len(msgs)} arrived, {med}"


def main():
    con = serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200, timeout=0.3)
    ble = BlePort(name)
    af_out, af_in = af_ports()
    for mode in modes:
        con.write(mode.encode())
        time.sleep(1.0)
        print(f"mode {mode}: {con.read(2000).decode(errors='replace').strip().splitlines()[-1:]}", flush=True)
        if wait:
            input(f"  read the meter in mode {mode} (connected, idle), then press Enter ")
        single = [[0x90, 60 + i, 100] for i in range(15)]
        print("  idle -> one message, BLE -> TRS:", run(ble, af_in, single, 1.5), flush=True)
        print("  idle -> one message, TRS -> BLE:", run(af_out, ble, single, 1.5), flush=True)
        stream = [[0xB0 | (i % 16), i // 16, i % 128] for i in range(200)]  # all different
        print("  stream 20 ms, BLE -> TRS:      ", run(ble, af_in, stream, 0.02), flush=True)
        print("  stream 20 ms, TRS -> BLE:      ", run(af_out, ble, stream, 0.02), flush=True)
    con.write(b"0")
    time.sleep(0.5)
    con.write(b"s")
    time.sleep(1)
    print(con.read(3000).decode(errors="replace"))
    ble.close()


if __name__ == "__main__":
    main()

"""One-way latency and integrity between a wireless CoreMIDI port and the AudioFuse, both on the Mac's clock.

    out:  Mac -> <port> (BLE / network) -> receiver -> J2 OUT -> AudioFuse MIDI IN -> Mac
    in:   Mac -> AudioFuse MIDI OUT -> J3 IN -> receiver -> <port> -> Mac

    uv run --with python-rtmidi python tools/hil/latency.py "<port name>" [count] [gap_ms]

Each message is a CC whose channel/controller/value encode its sequence number, so order, loss and duplicates
show up as well as timing. Sends are spaced gap_ms (default 20) +-5 ms apart so they do not lock to a radio
interval.
"""

import random
import statistics
import sys
import time

import rtmidi

AUDIOFUSE = "Audiofuse"


def cc(i):
    return [0xB0 | (i % 16), (i // 16) % 120, i % 128]


def seq_of(m):
    return (m[1] * 16 + (m[0] & 0x0F)) if len(m) == 3 and m[0] & 0xF0 == 0xB0 else None


def open_pair(name):
    out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
    for p in (out, inp):
        idx = next((i for i, n in enumerate(p.get_ports()) if name.lower() in n.lower()), None)
        if idx is None:
            sys.exit(f"no port matching '{name}' in {p.get_ports()}")
        p.open_port(idx)
    inp.ignore_types(sysex=True, timing=True, active_sense=True)
    return out, inp


def stats(lat_ms):
    lat = sorted(lat_ms)
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    return {"median": statistics.median(lat), "p95": p(0.95), "p99": p(0.99), "max": lat[-1], "min": lat[0],
            "stdev": statistics.pstdev(lat)}


def run(send, recv_port, n, gap):
    got, order = {}, []

    def cb(ev, _):
        s = seq_of(ev[0])
        if s is not None:
            order.append(s)
            got.setdefault(s, time.perf_counter())

    recv_port.set_callback(cb)
    sent = {}
    for i in range(n):
        sent[i] = time.perf_counter()
        send.send_message(cc(i))
        time.sleep(gap + random.uniform(-0.005, 0.005))
    time.sleep(1)
    recv_port.cancel_callback()
    lat = [(got[i] - sent[i]) * 1000 for i in sent if i in got]
    res = stats(lat) if lat else {}
    res.update(lost=n - len(lat), dup=len(order) - len(set(order)), reordered=order != sorted(order), n=n)
    return res


def fmt(name, r):
    if "median" not in r:
        return f"  {name}: nothing arrived"
    return (f"  {name}: median {r['median']:6.2f} ms  p95 {r['p95']:6.2f}  p99 {r['p99']:6.2f}  max {r['max']:6.2f}  "
            f"min {r['min']:5.2f}  stdev {r['stdev']:5.2f}  lost {r['lost']}/{r['n']}  dup {r['dup']}"
            f"{'  REORDERED' if r['reordered'] else ''}")


def measure(port, n=300, gap_ms=20):
    w_out, w_in = open_pair(port)
    a_out, a_in = open_pair(AUDIOFUSE)
    time.sleep(0.5)
    gap = gap_ms / 1000
    out = run(w_out, a_in, n, gap)
    inn = run(a_out, w_in, n, gap)
    for p in (w_out, w_in, a_out, a_in):
        p.close_port()
    return out, inn


if __name__ == "__main__":
    port = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    gap = float(sys.argv[3]) if len(sys.argv) > 3 else 20
    out, inn = measure(port, n, gap)
    print(f"{port}:")
    print(fmt("wireless -> TRS", out))
    print(fmt("TRS -> wireless", inn))
    sys.exit(0 if out["lost"] == 0 and inn["lost"] == 0 else 1)

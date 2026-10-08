"""Spike S2, Mac side: one-way BLE MIDI latency through the receiver, with its Wi-Fi off, idle, moderate and busy.

    BLE -> TRS: Mac (bleak, BLE MIDI write) -> receiver -> J2 OUT -> AudioFuse MIDI IN -> Mac (rtmidi)
    TRS -> BLE: Mac (rtmidi) -> AudioFuse MIDI OUT -> J3 IN -> receiver -> BLE notify -> Mac (bleak)

Both ends are on this Mac, so every time is one way on one clock. The Wi-Fi mode is switched over the receiver's
USB console. Each message is a CC whose channel/controller/value encode its sequence number.

    uv run --with bleak --with python-rtmidi --with pyserial python spikes/s2_ble_wifi/mac_latency.py [--bleak] [modes]

BLE goes through CoreMIDI's own Bluetooth driver (tools/hil/blemidi connects it), or with --bleak straight from
CoreBluetooth.

modes: off idle moderate busy (default: off, or all four once the firmware has Wi-Fi credentials)
"""

import asyncio
import glob
import random
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import rtmidi
import serial
from bleak import BleakClient, BleakScanner

NAME = "airmidi-s2"
MIDI_CHR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
N = 300  # messages per direction and mode
GAP = 0.02  # seconds between messages, with +-5 ms of random jitter so sends do not lock to the BLE interval


def cc(i):
    return [0xB0 | (i % 16), (i // 16) % 120, i % 128]


def seq_of(m):
    return (m[1] * 16 + (m[0] & 0x0F)) if len(m) == 3 and m[0] & 0xF0 == 0xB0 else None


def ble_packet(msg):
    ts = int(time.monotonic() * 1000) & 0x1FFF
    return bytes([0x80 | (ts >> 7), 0x80 | (ts & 0x7F)] + msg)


def ble_decode(data):
    """BLE MIDI packet -> list of MIDI messages (enough for the CCs this test sends)."""
    out, cur, after_ts = [], [], False
    for b in data[1:]:
        if b & 0x80 and not after_ts:
            after_ts = True
            continue
        if b & 0x80:
            if cur:
                out.append(cur)
            cur = [b]
        else:
            if not cur and out:  # running status
                cur = [out[-1][0]]
            cur.append(b)
            if len(cur) == 3:
                out.append(cur)
                cur = []
        after_ts = False
    if cur:
        out.append(cur)
    return out


def open_port(cls):
    port = cls()
    idx = next((i for i, n in enumerate(port.get_ports()) if "audiofuse" in n.lower()), None)
    if idx is None:
        sys.exit(f"no AudioFuse in {port.get_ports()}")
    port.open_port(idx)
    return port


def stats(name, sent, got):
    lat = [(got[i] - sent[i]) * 1000 for i in sent if i in got]
    lost = len(sent) - len(lat)
    if not lat:
        return f"  {name}: nothing arrived ({lost} lost)"
    lat.sort()
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
    return (f"  {name}: median {statistics.median(lat):6.2f} ms  p95 {p(0.95):6.2f}  max {lat[-1]:6.2f}  "
            f"min {lat[0]:5.2f}  stdev {statistics.pstdev(lat):5.2f}  lost {lost}/{len(sent)}")


class Console:
    def __init__(self):
        self.s = serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200, timeout=0.2)
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            line = self.s.readline().decode(errors="replace").strip()
            if line:
                self.lines.append(line)

    def cmd(self, c, wait_for=None, timeout=20):
        n0 = len(self.lines)
        self.s.write((c + "\n").encode())
        end = time.time() + timeout
        while wait_for and time.time() < end:
            if any(wait_for in l for l in self.lines[n0:]):
                return True
            time.sleep(0.05)
        time.sleep(0.3)
        return not wait_for


def mac_ip():
    return subprocess.run(["ipconfig", "getifaddr", "en0"], capture_output=True, text=True).stdout.strip()


class BleakLink:
    """BLE MIDI straight from CoreBluetooth (macOS keeps a generic 30 ms connection interval)."""

    def __init__(self, client):
        self.client = client

    async def send(self, msg):
        await self.client.write_gatt_char(MIDI_CHR, ble_packet(msg), response=False)

    async def start(self, cb):
        await self.client.start_notify(MIDI_CHR, lambda _, data: [cb(m) for m in ble_decode(data)])

    async def stop(self):
        await self.client.stop_notify(MIDI_CHR)


class CoreMidiLink:
    """The receiver as a CoreMIDI port, owned by Apple's BLE MIDI driver (tools/hil/blemidi connect)."""

    def __init__(self, port_name):
        self.out, self.inp = rtmidi.MidiOut(), rtmidi.MidiIn()
        for p in (self.out, self.inp):
            p.open_port(p.get_ports().index(port_name))
        self.inp.ignore_types(sysex=True, timing=True, active_sense=True)

    async def send(self, msg):
        self.out.send_message(msg)

    async def start(self, cb):
        self.inp.set_callback(lambda ev, _: cb(ev[0]))

    async def stop(self):
        self.inp.cancel_callback()


async def run_mode(link, midi_in, midi_out, con, mode, sink):
    if mode == "off":
        con.cmd("o")
    elif mode == "idle":
        if not con.cmd("i", "got ip") and not any("got ip" in l for l in con.lines):
            return f"{mode}: Wi-Fi did not join ({con.lines[-1:]})"
    else:
        con.cmd(("b" if mode == "busy" else "m") + mac_ip(), "got ip" if not any("got ip" in l for l in con.lines)
                else None)
    await asyncio.sleep(2)
    udp0 = sink["bytes"]

    # BLE -> TRS
    got_trs, sent = {}, {}
    midi_in.set_callback(lambda ev, _: got_trs.setdefault(seq_of(ev[0]), time.perf_counter()))
    for i in range(N):
        sent[i] = time.perf_counter()
        await link.send(cc(i))
        await asyncio.sleep(GAP + random.uniform(-0.005, 0.005))
    await asyncio.sleep(1)
    midi_in.cancel_callback()
    a = stats("BLE -> TRS", sent, got_trs)

    # TRS -> BLE
    got_ble, sent2 = {}, {}

    def on_msg(m):
        s = seq_of(m)
        if s is not None:
            got_ble.setdefault(s, time.perf_counter())

    await link.start(on_msg)
    await asyncio.sleep(0.5)
    for i in range(N):
        sent2[i] = time.perf_counter()
        midi_out.send_message(cc(i))
        await asyncio.sleep(GAP + random.uniform(-0.005, 0.005))
    await asyncio.sleep(1)
    await link.stop()
    b = stats("TRS -> BLE", sent2, got_ble)
    con.cmd("s", "status:")
    st = next((l for l in reversed(con.lines) if l.startswith("status:")), "")
    udp = (sink["bytes"] - udp0) / 1e3
    return f"{mode}:\n{a}\n{b}\n  receiver {st[8:]}\n  Mac received {udp:.0f} KB of UDP flood during the run"


async def main():
    args = sys.argv[1:]
    via = "bleak" if "--bleak" in args else "coremidi"
    args = [a for a in args if not a.startswith("--")]
    con = Console()
    con.cmd("s", "status:")
    has_wifi = not any("wifi none" in l for l in con.lines)
    modes = args or (["off", "idle", "moderate", "busy"] if has_wifi else ["off"])

    sink = {"bytes": 0}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 9999))

    def drain():
        while True:
            sink["bytes"] += len(sock.recv(2048))

    threading.Thread(target=drain, daemon=True).start()

    midi_out, midi_in = open_port(rtmidi.MidiOut), open_port(rtmidi.MidiIn)
    midi_in.ignore_types(sysex=True, timing=True, active_sense=True)
    print(f"BLE via {via}")
    if via == "coremidi":
        tool = Path(__file__).parents[2] / "tools/hil/build/blemidi"
        subprocess.run([str(tool), "connect", NAME], check=True, capture_output=True)
        await asyncio.sleep(1.5)
        link = CoreMidiLink(f"{NAME} Bluetooth")
        print("receiver:", *[l for l in con.lines if "interval" in l or "MTU" in l][-3:], sep="\n  ")
        for mode in modes:
            print(await run_mode(link, midi_in, midi_out, con, mode, sink), flush=True)
    else:
        dev = await BleakScanner.find_device_by_name(NAME, timeout=10)
        if not dev:
            sys.exit(f"{NAME} not found")
        async with BleakClient(dev) as client:
            await asyncio.sleep(1.5)  # let the connection parameter update settle
            print("receiver:", *[l for l in con.lines if "interval" in l or "MTU" in l][-3:], sep="\n  ")
            for mode in modes:
                print(await run_mode(BleakLink(client), midi_in, midi_out, con, mode, sink), flush=True)
    con.cmd("o")


if __name__ == "__main__":
    asyncio.run(main())

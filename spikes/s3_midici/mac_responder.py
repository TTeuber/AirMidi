"""Spike S3, Mac side: play a MIDI-CI device on the AudioFuse ports. Answers Discovery, PE capabilities and Get
Property Data for ResourceList, DeviceInfo and AllCtrlList from hard-coded JSON, chunked to the initiator's
Receivable Maximum SysEx Message Size. Asks the receiver (over its USB console) to run a scan, and prints the
receiver's console so both sides of the conversation show up in one place.

    uv run --with python-rtmidi --with pyserial python spikes/s3_midici/mac_responder.py [scans]
"""

import glob
import json
import random
import sys
import threading
import time

import rtmidi
import serial

PORT_NAME = "Audiofuse"
VERSION = 0x02
MUID = random.randrange(0x0FFFFF00)

RESOURCES = {
    "ResourceList": [{"resource": "DeviceInfo"}, {"resource": "AllCtrlList"}],
    "DeviceInfo": {
        "manufacturerId": [125, 0, 0],
        "manufacturer": "Educational Use",
        "familyId": [0, 0],
        "family": "airmidi",
        "modelId": [2, 0],
        "model": "Mac stand-in",
        "versionId": [0, 0, 1, 0],
        "version": "0.1",
    },
    "AllCtrlList": [
        {"title": "Cutoff", "channel": 1, "ctrlType": "cc", "ctrlIndex": [74], "priority": 1,
         "default": 2147483648, "paramPath": "/filter/cutoff"},
        {"title": "Resonance", "channel": 1, "ctrlType": "cc", "ctrlIndex": [71], "priority": 1,
         "default": 0, "paramPath": "/filter/resonance"},
        {"title": "Attack", "channel": 1, "ctrlType": "cc", "ctrlIndex": [73], "priority": 2,
         "default": 0, "paramPath": "/env/attack"},
        {"title": "Decay", "channel": 1, "ctrlType": "cc", "ctrlIndex": [75], "priority": 2,
         "default": 1073741824, "paramPath": "/env/decay"},
        {"title": "Release", "channel": 1, "ctrlType": "cc", "ctrlIndex": [72], "priority": 2,
         "default": 1073741824, "paramPath": "/env/release"},
        {"title": "Waveform", "channel": 1, "ctrlType": "cc", "ctrlIndex": [70], "priority": 1,
         "default": 0, "stepCount": 2, "typeHint": "valueSelect", "ctrlMapId": "wave", "paramPath": "/osc/wave"},
    ],
}


def muid_bytes(m):
    return [(m >> (7 * i)) & 0x7F for i in range(4)]


def muid_of(b):
    return b[0] | b[1] << 7 | b[2] << 14 | b[3] << 21


def b14(v):
    return [v & 0x7F, (v >> 7) & 0x7F]


def header(sub2, dst):
    return [0xF0, 0x7E, 0x7F, 0x0D, sub2, VERSION] + muid_bytes(MUID) + muid_bytes(dst)


class Responder:
    def __init__(self, midi_out):
        self.out = midi_out
        self.max_sysex = {}  # initiator MUID -> its receivable max SysEx size
        self.log = []

    def send(self, m):
        self.out.send_message(m)

    def handle(self, m):
        if len(m) < 15 or m[1] != 0x7E or m[3] != 0x0D:
            return
        sub2, src, dst = m[4], muid_of(m[6:10]), muid_of(m[10:14])
        d = m[14:-1]
        if sub2 == 0x70 and dst == 0x0FFFFFFF:
            self.max_sysex[src] = muid_of(d[12:16]) if len(d) >= 16 else 128
            out_path = d[16] if len(d) > 16 else 0
            r = header(0x71, src) + [0x7D, 0, 0] + b14(0) + b14(2) + [0, 0, 1, 0, 1 << 3] + muid_bytes(512)
            self.send(r + [out_path, 0x7F, 0xF7])
            self.log.append(f"Discovery from {src:07X} (max SysEx {self.max_sysex[src]}), replied")
        elif dst != MUID:
            return
        elif sub2 == 0x30:
            self.send(header(0x31, src) + [1, 0, 0, 0xF7])
            self.log.append("PE capabilities, replied")
        elif sub2 == 0x34:
            req, hl = d[0], d[1] | d[2] << 7
            hdr = json.loads(bytes(d[3:3 + hl]))
            res = hdr.get("resource")
            if res in RESOURCES:
                body = json.dumps(RESOURCES[res], separators=(",", ":")).encode()
                status = {"status": 200}
            else:
                body, status = b"", {"status": 404, "message": "unknown resource"}
            self.send_chunks(src, req, json.dumps(status, separators=(",", ":")).encode(), body)
            self.log.append(f"Get {res}: {len(body)} bytes")

    def send_chunks(self, dst, req, hdr, body):
        """Reply to Get Property Data, split so no message exceeds the initiator's max SysEx size."""
        fixed = 14 + 1 + 2 + len(hdr) + 6 + 1
        room = self.max_sysex.get(dst, 128) - fixed
        parts = [body[i:i + room] for i in range(0, len(body), room)] or [b""]
        for i, part in enumerate(parts, 1):
            h = hdr if i == 1 else b""
            m = header(0x35, dst) + [req] + b14(len(h)) + list(h)
            m += b14(len(parts)) + b14(i) + b14(len(part)) + list(part) + [0xF7]
            self.send(m)


def open_port(cls):
    port = cls()
    idx = next((i for i, n in enumerate(port.get_ports()) if PORT_NAME.lower() in n.lower()), None)
    if idx is None:
        sys.exit(f"no '{PORT_NAME}' port in {port.get_ports()}")
    port.open_port(idx)
    return port


def main():
    scans = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    midi_out, midi_in = open_port(rtmidi.MidiOut), open_port(rtmidi.MidiIn)
    midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
    r = Responder(midi_out)
    midi_in.set_callback(lambda ev, _: r.handle(ev[0]))
    con = serial.Serial(glob.glob("/dev/cu.usbmodem*")[0], 115200, timeout=0.2)
    lines = []

    def read_console():
        while True:
            line = con.readline().decode(errors="replace").rstrip()
            if line:
                lines.append(line)

    threading.Thread(target=read_console, daemon=True).start()
    print(f"responder MUID {MUID:07X} on the AudioFuse ports")
    for _ in range(scans):
        n0 = len(lines)
        con.write(b"d")
        deadline = time.time() + 15
        while time.time() < deadline and not any(l.startswith("== done") or "FAILED" in l or "no reply" in l
                                                 for l in lines[n0:]):
            time.sleep(0.05)
        time.sleep(0.2)
        for line in r.log:
            print("  mac |", line)
        r.log.clear()
        for line in lines[n0:]:
            print("  rx  |", line)
    sys.exit(0 if any(l.startswith("== done") for l in lines) else 1)


if __name__ == "__main__":
    main()

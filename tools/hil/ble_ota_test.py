"""Firmware update over BLE on the real box, in Play mode (Wi-Fi off: BLE is how you would update it there).

    uv run --with bleak --with pyserial python tools/hil/ble_ota_test.py <host> [--no-build]

Builds a labelled image (version "ota-ble"), puts the box in Play mode, and over BLE (tools/hil/ble_ota.py):
  - an unsigned image and a corrupted one are rejected, and the box keeps running what it ran;
  - the good image is accepted, the box restarts into it (its version characteristic says "ota-ble"), and it is
    still running it after the self-test (no rollback);
then back to Network mode over the console, where the web API must show the image as valid.
"""

import asyncio
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).parent))
import ble_ota  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "firmware"
host = sys.argv[1]
name = "airmidi-7530"
results = []


def check(what, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {what}{': ' + str(detail) if detail else ''}", flush=True)


def status():
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=3) as r:
            return json.load(r)
    except Exception:
        return None


def console(cmd):
    s = serial.Serial()
    s.port, s.baudrate = "/dev/cu.usbmodem1101", 115200
    s.dtr = s.rts = False  # set before opening: DTR high would select download mode, RTS resets
    s.open()
    time.sleep(0.5)
    s.write(cmd)
    s.flush()
    time.sleep(0.5)
    s.close()


out = FW / "build_c6_ota_ble"
if "--no-build" not in sys.argv:
    print("building ota-ble ...", flush=True)
    env = {"BUILD_DIR": out.name, "PROJECT_VER": "ota-ble", "PATH": subprocess.os.environ["PATH"],
           "HOME": subprocess.os.environ["HOME"]}
    r = subprocess.run([str(FW / "build.sh"), "c6"], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"build failed:\n{r.stdout[-2000:]}")
good = (out / "airmidi.bin").read_bytes()
unsigned = (out / "airmidi-unsigned.bin").read_bytes()
corrupt = bytearray(good)
for i in range(len(corrupt) // 2, len(corrupt) // 2 + 64):
    corrupt[i] ^= 0xFF

s = status()
if not s:
    sys.exit(f"no answer from http://{host}/api/status")
before = s["version"]
print(f"box runs {before}; to Play mode", flush=True)
try:
    urllib.request.urlopen(urllib.request.Request(f"http://{host}/api/mode", data=b'{"mode": "play"}',
                                                  headers={"content-type": "application/json"}), timeout=3)
except OSError:
    pass  # Wi-Fi may go before the reply does
time.sleep(5)


async def run():
    check("version over BLE", (v := await ble_ota.version(name)) == before, v)
    for label, data in [("unsigned image", unsigned), ("corrupted image", bytes(corrupt))]:
        result, took, rate = await ble_ota.upload(name, data)
        check(f"{label} rejected", result.startswith("failed"), result)
        check(f"{label}: box runs what it ran", (v := await ble_ota.version(name)) == before, v)
    result, took, rate = await ble_ota.upload(name, good)
    check("good image accepted", result == "accepted", f"{len(good)} bytes in {took:.1f} s, {rate / 1024:.1f} KB/s")
    await asyncio.sleep(8)
    check("box restarted into it", (v := await ble_ota.version(name)) == "ota-ble", v)
    await asyncio.sleep(15)  # past the self-test (10 s)
    check("still running it after the self-test", (v := await ble_ota.version(name)) == "ota-ble", v)


asyncio.run(run())
console(b"N")
for _ in range(40):
    s = status()
    if s and s["mode"] == "network":
        break
    time.sleep(1)
check("network mode: the web API shows it valid", s and s["version"] == "ota-ble" and s["ota"]["state"] == "valid",
      s and (s["version"], s["ota"]["state"]))
print("ALL PASS" if all(results) else "FAILED")
sys.exit(0 if all(results) else 1)

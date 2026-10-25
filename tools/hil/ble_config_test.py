"""The BLE config service on the real box: the JSON API over BLE, Wi-Fi setup from a phone, the pairing window.

    uv run --with bleak --with pyserial python tools/hil/ble_config_test.py <ble name> <console port>

Start with the box in Setup mode (console 'S'), so this Mac may pair. Uses the dev network from the repo's .wifi
(line 1 SSID, line 2 password; never printed). Checks:
  - requests and replies of every size, errors, unknown paths
  - a Wi-Fi scan, a wrong password, the right one (stored), all over BLE, still in Setup mode
  - Play mode: the scan and the try work there too (the radio comes on and goes off again)
  - forgetting the trusted phones: after a restart (window closed) the Mac is refused with ATT error 0x08, and
    Setup mode lets it back in
"""

import asyncio
import pathlib
import sys
import time

import serial
from bleak.exc import BleakError

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from blecfg import BleConfig  # noqa: E402

name, port = sys.argv[1], sys.argv[2]
ssid, password = (pathlib.Path(__file__).parents[2] / ".wifi").read_text().splitlines()[:2]
results = []


def check(label, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{': ' + str(detail) if detail != '' else ''}")


def console(cmd, wait=8):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False  # DTR high would select download mode, RTS resets
    s.open()
    s.write(cmd.encode())
    end, out = time.time() + wait, b""
    while time.time() < end:
        out += s.read(4096)
    s.close()
    return out.decode(errors="replace")


async def poll(c, path, done, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        status, r = await c.call("GET", path)
        if done(r):
            return r
        await asyncio.sleep(0.5)
    return r


async def setup_mode_checks():
    async with BleConfig(name) as c:
        print(f"connected, mtu {c.client.mtu_size}")
        status, s = await c.call("GET", "/api/status")
        check("status over BLE (pairs first)", status == 200 and s.get("mode") == "setup", f"{len(str(s))} chars")
        status, p = await c.call("GET", "/api/phones")
        check("this Mac is trusted now", status == 200 and p["trusted"] >= 1 and p["window"] == "setup", p)
        status, r = await c.call("GET", "/api/nothing")
        check("unknown path: 404", status == 404, r)
        status, r = await c.call("PUT", "/api/status")
        check("wrong method: 405", status == 405, r)
        status, r = await c.call("POST", "/api/settings", b"not json")
        check("bad body: 400", status == 400, r)
        status, r = await c.call("POST", "/api/ci/profile", b"[" + b" " * 9000 + b"]")
        check("a request beyond the buffer: 413", status == 413, r)

        status, _ = await c.call("POST", "/api/wifi/scan")
        r = await poll(c, "/api/wifi/scan", lambda r: r["state"] != "scanning")
        names = [n["ssid"] for n in r.get("networks", [])]
        check("scan in Setup mode", r["state"] == "scanned" and ssid in names, f"{len(names)} networks")

        status, _ = await c.call("POST", "/api/wifi/try", {"ssid": ssid, "pass": "not-the-password"})
        r = await poll(c, "/api/wifi/try", lambda r: r["state"] not in ("joining",))
        check("wrong password is reported", r["state"] == "failed" and "password" in r["reason"], r["reason"])

        status, _ = await c.call("POST", "/api/wifi/try", {"ssid": ssid, "pass": password})
        r = await poll(c, "/api/wifi/try", lambda r: r["state"] not in ("joining",))
        check("the right password joins", r["state"] == "joined" and r["ip"], r["ip"])
        status, w = await c.call("GET", "/api/wifi")
        check("and the network is stored", w["networks"][:1] == [ssid])
        status, s = await c.call("GET", "/api/status")
        check("still in Setup mode afterwards", s["mode"] == "setup" and s["wifi"]["state"] == "access point")

        await c.call("POST", "/api/mode", {"mode": "play"})  # no restart: BLE stays on


async def play_mode_checks():
    await asyncio.sleep(2)
    async with BleConfig(name) as c:
        status, s = await c.call("GET", "/api/status")
        check("play mode, Wi-Fi off", s["mode"] == "play" and s["wifi"]["state"] == "off")
        t0 = time.time()
        await c.call("POST", "/api/wifi/scan")
        r = await poll(c, "/api/wifi/scan", lambda r: r["state"] != "scanning")
        check("scan in Play mode", r["state"] == "scanned" and r["networks"], f"{time.time() - t0:.1f} s")
        await c.call("POST", "/api/wifi/try", {"ssid": ssid, "pass": password})
        r = await poll(c, "/api/wifi/try", lambda r: r["state"] not in ("joining",))
        check("try in Play mode", r["state"] == "joined", r["ip"] or r["reason"])
        status, s = await c.call("GET", "/api/status")
        check("radio off again", s["wifi"]["state"] == "off")
        status, p = await c.call("DELETE", "/api/phones")
        check("forget phones", status == 200 and p["trusted"] == 0, p)


async def refused_checks():
    # Two software restarts (Network mode stops BLE, Play starts it again): the window stays shut, as it only opens
    # by itself after a power-on.
    out = console("N", 12)
    out += console("P", 12)
    check("box back in Play mode", "mode: play" in out)
    try:
        async with BleConfig(name) as c:
            status, s = await c.call("GET", "/api/status")
            check("untrusted Mac refused (0x08)", False, f"got {status}")
    except BleakError as e:
        check("untrusted Mac refused (0x08)", "8" in str(e) or "authoriz" in str(e).lower(), str(e)[:80])
    console("S", 6)
    async with BleConfig(name) as c:
        status, s = await c.call("GET", "/api/status")
        check("Setup mode lets it back in", status == 200)
        await c.call("POST", "/api/mode", {"mode": "play"})


async def main():
    await setup_mode_checks()
    await play_mode_checks()
    await refused_checks()
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)


asyncio.run(main())

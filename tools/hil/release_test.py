"""Update from a published release, the way the box does it from GitHub, but from a local web server.

    uv run python tools/hil/release_test.py <box host> [--no-build]

Builds an image labelled v0.1.0-test, serves it with a manifest.json from this Mac, asks the box to check
(must report it as newer) and install it (must download with progress, restart into it and confirm it).
"""

import functools
import http.server
import json
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
host = sys.argv[1]
results = []
VERSION = "v0.1.0-test"


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}", flush=True)


def api(method, path, body=None):
    req = urllib.request.Request(f"http://{host}{path}", method=method, data=json.dumps(body or {}).encode(),
                                 headers={"content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")
    except Exception as e:
        return 0, {"error": str(e)}


def my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((host, 80))
    return s.getsockname()[0]


def main():
    out = ROOT / "firmware" / "build_c6_release_test"
    if "--no-build" not in sys.argv:
        print(f"building {VERSION} ...", flush=True)
        env = {"BUILD_DIR": out.name, "PROJECT_VER": VERSION, "PATH": subprocess.os.environ["PATH"],
               "HOME": subprocess.os.environ["HOME"]}
        r = subprocess.run([str(ROOT / "firmware" / "build.sh"), "c6"], env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout[-2000:])
    site = Path("/tmp/airmidi-release")
    shutil.rmtree(site, ignore_errors=True)
    site.mkdir()
    shutil.copy(out / "airmidi.bin", site / "airmidi-c6.bin")
    (site / "manifest.json").write_text(json.dumps({"version": VERSION, "images": {"c6": "airmidi-c6.bin"}}))
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(site))
    server = http.server.ThreadingHTTPServer(("0.0.0.0", 8765), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://{my_ip()}:8765"

    code, r = api("POST", "/api/update/check", {"base": base})
    check("check sees the release as newer", code == 200 and r.get("latest") == VERSION and r.get("newer"), r)
    code, r = api("POST", "/api/update/check", {"base": base + "/nothing-here"})
    check("a missing manifest is reported", code == 502, r)
    code, r = api("POST", "/api/update/install", {"base": base})
    check("install started", code == 202, r)
    seen, end = set(), time.time() + 120
    while time.time() < end:
        c, s = api("GET", "/api/status")
        if c == 200:
            f = s["ota"]["fetch"]
            seen.add(f["percent"] // 10)
            if s["version"] == VERSION:
                break
            if f["state"] == "failed":
                break
        time.sleep(1)
    check("download progress reported", len(seen) >= 3, sorted(seen))
    c, s = api("GET", "/api/status")
    check("restarted into the release", c == 200 and s["version"] == VERSION, c == 200 and s["ota"])
    end = time.time() + 30
    while time.time() < end and not (c == 200 and s["ota"]["state"] == "valid"):
        time.sleep(1)
        c, s = api("GET", "/api/status")
    check("self-test confirmed it", c == 200 and s["ota"]["state"] == "valid", c == 200 and s["ota"])
    code, r = api("POST", "/api/update/check", {"base": base})
    check("now up to date", code == 200 and not r.get("newer"), r)
    print("ALL PASS" if all(results) else f"FAILED ({results.count(False)} of {len(results)})")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()

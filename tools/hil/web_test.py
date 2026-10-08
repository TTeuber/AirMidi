"""The web UI's API on the real box (Network mode), without changing anything that would restart it.

    uv run python tools/hil/web_test.py <host>
"""

import json
import sys
import urllib.error
import urllib.request

host = sys.argv[1]
results = []


def call(method, path, body=None, raw=False):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"http://{host}{path}", data=data, method=method,
                                 headers={"content-type": "application/json", "accept-encoding": "gzip"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, (r.headers, r.read()) if raw else json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}")


code, s = call("GET", "/api/status")
check("status", code == 200 and {"version", "wifi", "battery", "ports", "ota"} <= s.keys(), s.get("version"))
for path in ["/", "/app.js", "/style.css", "/geist-medium.woff2"]:
    code, (headers, body) = call("GET", path, raw=True)
    check(f"GET {path} gzipped", code == 200 and headers.get("Content-Encoding") == "gzip", f"{len(body)} bytes")
code, st = call("GET", "/api/settings")
check("settings", code == 200 and st.get("name") == s["name"], st)
code, r = call("POST", "/api/settings", {**st, "name": "bad name!"})
check("invalid name rejected", code == 400, r.get("error"))
code, r = call("POST", "/api/settings", {**st, "sysex_timeout_ms": 10})
check("timeout out of range rejected", code == 400, r.get("error"))
code, r = call("POST", "/api/mode", {"mode": "party"})
check("unknown mode rejected", code == 400, r.get("error"))
code, r = call("POST", "/api/wifi", {"ssid": "airmidi-test-net", "pass": "x" * 70})
check("password too long rejected", code == 400, r.get("error"))
code, r = call("POST", "/api/wifi", {"ssid": "airmidi test net", "pass": "secret123"})
check("add network", code == 200 and "airmidi test net" in r["networks"], r)
check("passwords never come back", "secret123" not in json.dumps(r))
code, r = call("DELETE", "/api/wifi?ssid=airmidi%20test%20net")
check("forget network", code == 200 and "airmidi test net" not in r["networks"], r)
code, r = call("POST", "/api/update", None)
check("empty update rejected", code == 400, r.get("error"))
print("ALL PASS" if all(results) else f"FAILED ({results.count(False)})")
sys.exit(0 if all(results) else 1)

"""Over-the-air update and rollback on the real box, through the web API.

    uv run python tools/hil/ota_test.py <host> [--no-build]

Builds the images it needs (firmware/build.sh with separate build directories), then uploads them in turn:

  rejected outright (the box keeps running what it has):
    - a corrupted image (bytes flipped in the middle)
    - the unsigned image ESP-IDF leaves next to the signed one
    - an image signed with a different key
  accepted, then judged by the box itself after it restarts into them:
    - a good image: runs "pending verify", passes its self-test after 10 s, stays (valid)
    - an image whose self-test fails: rolls back to the good one
    - an image that crashes 3 s after boot: the bootloader rolls back to the good one, and a core dump is left

Ends on the good image. The box must be in Network mode and running a build signed with the dev key.
"""

import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "firmware"
host = sys.argv[1]
build = "--no-build" not in sys.argv
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{': ' + str(detail) if detail else ''}", flush=True)


def status(timeout=4):
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
            return json.loads(r.read())
    except Exception:
        return None


def upload(path):
    # curl, not urllib: urllib's one-shot send of a 1.5 MB body broke the pipe against esp_http_server
    t0 = time.time()
    r = subprocess.run(["curl", "-s", "-m", "300", "-X", "POST", "--data-binary", f"@{path}", "-w", "\n%{http_code}",
                        "-H", "content-type: application/octet-stream", f"http://{host}/api/update"],
                       capture_output=True, text=True)
    body, _, code = r.stdout.rpartition("\n")
    return int(code or 0), body, time.time() - t0


def wait_for(pred, timeout, what):
    """Poll status until pred(status) holds; returns the status or None."""
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if s and pred(s):
            return s
        time.sleep(1)
    print(f"    (timed out waiting for {what}; last: {s and (s['version'], s['ota'])})", flush=True)
    return None


def build_image(name, extra=None, key=None):
    out = FW / f"build_c6_ota_{name}"
    env = {"BUILD_DIR": out.name, "PROJECT_VER": f"ota-{name}", "PATH": subprocess.os.environ["PATH"],
           "HOME": subprocess.os.environ["HOME"]}
    if extra:
        env["EXTRA_DEFAULTS"] = f"test/sdkconfig.{extra}"
    if build:
        print(f"building ota-{name} ...", flush=True)
        r = subprocess.run([str(FW / "build.sh"), "c6"], env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"build of {name} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return out


def main():
    s = status()
    if not s:
        sys.exit(f"no answer from http://{host}/api/status")
    print(f"box runs {s['version']} from {s['ota']['running']} ({s['ota']['state']})", flush=True)

    other_key = FW / "keys" / "other_test_key.pem"
    if not other_key.exists():
        subprocess.run(["espsecure.py", "generate_signing_key", "--version", "2", "--scheme", "rsa3072",
                        str(other_key)], check=True, capture_output=True)
    good = build_image("good")
    fail = build_image("selftest-fail", "selftest_fail")
    crash = build_image("crash", "crash")
    wrong = build_image("wrong-key", "other_key")

    corrupt = Path("/tmp/airmidi-corrupt.bin")
    data = bytearray((good / "airmidi.bin").read_bytes())
    for i in range(len(data) // 2, len(data) // 2 + 64):
        data[i] ^= 0xFF
    corrupt.write_bytes(data)

    print("rejected uploads", flush=True)
    # The unsigned image is one whose content was never uploaded signed, so no leftover signature could match it.
    for name, path in [("corrupted image", corrupt), ("unsigned image", fail / "airmidi-unsigned.bin"),
                       ("image signed with another key", wrong / "airmidi.bin")]:
        code, body, dt = upload(path)
        check(f"{name} rejected", code == 400, f"{code} {body.strip()} ({dt:.1f} s)")
    s2 = wait_for(lambda s: True, 30, "the box")
    check("box unchanged after the rejections", s2 and s2["version"] == s["version"], s2 and s2["version"])

    print("good image", flush=True)
    before = s2["ota"]["running"]
    code, body, dt = upload(good / "airmidi.bin")
    size = (good / "airmidi.bin").stat().st_size
    check("good image accepted", code == 200, f"{size / 1024:.0f} KB in {dt:.1f} s ({size / 1024 / dt:.0f} KB/s)")
    s3 = wait_for(lambda s: s["version"] == "ota-good", 60, "the good image to boot")
    check("restarted into it", bool(s3) and s3["ota"]["running"] != before, s3 and s3["ota"])
    s3 = wait_for(lambda s: s["ota"]["state"] == "valid", 30, "the self-test")
    check("self-test confirmed it (valid)", bool(s3), s3 and s3["ota"])
    good_slot = s3 and s3["ota"]["running"]

    print("image that fails its self-test", flush=True)
    code, body, dt = upload(fail / "airmidi.bin")
    check("accepted for a trial", code == 200, body.strip())
    s4 = wait_for(lambda s: s["version"] == "ota-selftest-fail", 60, "the failing image to boot")
    check("booted pending verify", bool(s4) and s4["ota"]["state"] == "pending verify", s4 and s4["ota"])
    s4 = wait_for(lambda s: s["version"] == "ota-good" and s["ota"]["rolled_back"], 60, "the rollback")
    check("rolled back to the good image", bool(s4) and s4["ota"]["running"] == good_slot, s4 and s4["ota"])

    print("image that crashes", flush=True)
    urllib.request.urlopen(urllib.request.Request(f"http://{host}/api/coredump", method="DELETE"), timeout=10)
    check("old core dump cleared", not status()["ota"]["coredump"]["present"])
    code, body, dt = upload(crash / "airmidi.bin")
    check("accepted for a trial", code == 200, body.strip())
    time.sleep(5)
    s5 = wait_for(lambda s: s["version"] == "ota-good" and s["ota"]["coredump"]["present"], 90, "the crash rollback")
    check("rolled back after the crash, core dump kept", bool(s5) and s5["ota"]["running"] == good_slot,
          s5 and s5["ota"])

    print("ALL PASS" if all(results) else f"FAILED ({results.count(False)} of {len(results)})")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()

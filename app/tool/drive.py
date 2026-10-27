# Drive a debug build of the app through its Dart VM service (lib/debug_drive.dart): screenshots and taps, for the
# desktop app where the terminal cannot capture the screen.
#   uv run --with websocket-client python tool/drive.py <vm-service-url> shot out.png
#   uv run --with websocket-client python tool/drive.py <vm-service-url> tap X Y      (rtap X Y: a right click)
#   uv run --with websocket-client python tool/drive.py <vm-service-url> drag X0 Y0 X1 Y1
#   uv run --with websocket-client python tool/drive.py <vm-service-url> keys "a s d+ d- tab+ tab-"
#     each word: a key pressed and released, or key+ (down) / key- (up); letters, ; ' and tab
# The URL is the "Dart VM service is listening on http://127.0.0.1:PORT/TOKEN=/" line the app prints at start.
import json
import shutil
import sys

import websocket


def call(ws, method, params, n=[0]):
    n[0] += 1
    ws.send(json.dumps({"jsonrpc": "2.0", "id": n[0], "method": method, "params": params}))
    while True:
        r = json.loads(ws.recv())
        if r.get("id") == n[0]:
            if "error" in r:
                raise SystemExit(f"{method}: {r['error']}")
            return r["result"]


# USB HID usages and Flutter logical key ids
HID = {**{c: 0x00070004 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")}, ";": 0x00070033, "'": 0x00070034,
       "tab": 0x0007002B}
LOGICAL = {**{c: ord(c) for c in "abcdefghijklmnopqrstuvwxyz;'"}, "tab": 0x0100000009}


def key(ws, iso, k, down):
    call(ws, "ext.airmidi.key", {"isolateId": iso, "physical": str(HID[k]), "logical": str(LOGICAL[k]),
                                 "down": "1" if down else "0", **({"char": k} if down and len(k) == 1 else {})})


def main():
    url, cmd = sys.argv[1], sys.argv[2]
    ws = websocket.create_connection(url.replace("http://", "ws://").rstrip("/") + "/ws")
    iso = call(ws, "getVM", {})["isolates"][0]["id"]
    if cmd == "shot":
        r = call(ws, "ext.airmidi.shot", {"isolateId": iso})
        shutil.copy(r["path"], sys.argv[3])
        print(sys.argv[3], r["w"], "x", r["h"])
    elif cmd == "tap":
        call(ws, "ext.airmidi.tap", {"isolateId": iso, "x": sys.argv[3], "y": sys.argv[4]})
    elif cmd == "rtap":
        call(ws, "ext.airmidi.tap", {"isolateId": iso, "x": sys.argv[3], "y": sys.argv[4], "buttons": "2"})
    elif cmd == "drag":
        call(ws, "ext.airmidi.drag", {"isolateId": iso, "x0": sys.argv[3], "y0": sys.argv[4], "x1": sys.argv[5],
                                      "y1": sys.argv[6]})
    elif cmd == "keys":
        import time
        for w in sys.argv[3].split():
            if w.endswith("+") or w.endswith("-"):
                key(ws, iso, w[:-1], w.endswith("+"))
            else:
                key(ws, iso, w, True)
                time.sleep(0.08)
                key(ws, iso, w, False)
            time.sleep(0.12)
    else:
        raise SystemExit(f"unknown command {cmd}")


main()

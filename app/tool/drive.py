# Drive a debug build of the app through its Dart VM service (lib/debug_drive.dart): screenshots and taps, for the
# desktop app where the terminal cannot capture the screen.
#   uv run --with websocket-client python tool/drive.py <vm-service-url> shot out.png
#   uv run --with websocket-client python tool/drive.py <vm-service-url> tap X Y
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
    else:
        raise SystemExit(f"unknown command {cmd}")


main()

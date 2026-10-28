# Drive the app through lib/shared/debug_drive.dart: screenshots and taps, for the desktop app where the terminal
# cannot capture the screen or send input. <url> is a debug build's Dart VM service, or the screenshot build's
# http://127.0.0.1:47800/drive (tool/shots_main.dart: a release build, which has no VM service).
#   uv run --with websocket-client python tool/drive.py <url> shot out.png
#   uv run --with websocket-client python tool/drive.py <url> tap X Y      (rtap X Y: a right click)
#   uv run --with websocket-client python tool/drive.py <url> tapText "arrangement"   (the first text that reads so)
#   uv run --with websocket-client python tool/drive.py <url> drag X0 Y0 X1 Y1
#   uv run --with websocket-client python tool/drive.py <url> keys "a s d+ d- tab+ tab-"
#     each word: a key pressed and released, or key+ (down) / key- (up); letters, digits, ; ' tab space backspace escape,
#     the arrows (up down left right) and meta / shift / alt (hold one with "meta+ a meta-")
#   uv run --with websocket-client python tool/drive.py <url> text "42"   (the focused text field's text)
# A debug build's URL is the "Dart VM service is listening on http://127.0.0.1:PORT/TOKEN=/" line it prints at start.
import json
import shutil
import sys
import urllib.error
import urllib.parse
import urllib.request


class Vm:
    """A debug build: the commands are VM service extensions (ext.airmidi.<command>) on the app's isolate."""

    def __init__(self, url):
        import websocket
        self.ws = websocket.create_connection(url.replace("http://", "ws://").rstrip("/") + "/ws")
        self.n = 0
        self.iso = self.rpc("getVM", {})["isolates"][0]["id"]

    def rpc(self, method, params):
        self.n += 1
        self.ws.send(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.n:
                if "error" in r:
                    raise SystemExit(f"{method}: {r['error']}")
                return r["result"]

    def __call__(self, command, params):
        return self.rpc(f"ext.airmidi.{command}", {"isolateId": self.iso, **params})


class Http:
    """The screenshot build: GET <url>/<command>?params (POST with a body for its own extra commands)."""

    def __init__(self, url):
        self.url = url.rstrip("/")

    def __call__(self, command, params, body=None):
        url = f"{self.url}/{command}?{urllib.parse.urlencode(params)}"
        try:
            with urllib.request.urlopen(urllib.request.Request(url, data=body), timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            raise SystemExit(f"{command}: {json.load(e).get('error')}")


def connect(url):
    return Http(url) if url.rstrip("/").endswith("/drive") else Vm(url)


# USB HID usages and Flutter logical key ids
HID = {**{c: 0x00070004 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")},
       **{c: 0x0007001E + i for i, c in enumerate("1234567890")}, ";": 0x00070033, "'": 0x00070034,
       "tab": 0x0007002B, "space": 0x0007002C, "backspace": 0x0007002A, "escape": 0x00070029, "right": 0x0007004F,
       "left": 0x00070050, "down": 0x00070051, "up": 0x00070052, "meta": 0x000700E3, "shift": 0x000700E1,
       "alt": 0x000700E2}
LOGICAL = {**{c: ord(c) for c in "abcdefghijklmnopqrstuvwxyz;'0123456789"}, "tab": 0x0100000009, "space": 0x20,
           "backspace": 0x0100000008, "escape": 0x010000001B, "right": 0x0100000303, "left": 0x0100000302,
           "down": 0x0100000301, "up": 0x0100000304, "meta": 0x0200000106, "shift": 0x0200000102, "alt": 0x0200000104}


def key(call, k, down):
    call("key", {"physical": str(HID[k]), "logical": str(LOGICAL[k]),
                                 "down": "1" if down else "0", **({"char": k} if down and len(k) == 1 else {})})


def main():
    url, cmd = sys.argv[1], sys.argv[2]
    call = connect(url)
    if cmd == "shot":
        r = call("shot", {})
        shutil.copy(r["path"], sys.argv[3])
        print(sys.argv[3], r["w"], "x", r["h"])
    elif cmd == "tap":
        call("tap", {"x": sys.argv[3], "y": sys.argv[4]})
    elif cmd == "tapText":
        call("tapText", {"text": sys.argv[3]})
    elif cmd == "rtap":
        call("tap", {"x": sys.argv[3], "y": sys.argv[4], "buttons": "2"})
    elif cmd == "drag":
        call("drag", {"x0": sys.argv[3], "y0": sys.argv[4], "x1": sys.argv[5],
                                      "y1": sys.argv[6]})
    elif cmd == "text":
        call("text", {"text": sys.argv[3]})
    elif cmd == "keys":
        import time
        for w in sys.argv[3].split():
            if w.endswith("+") or w.endswith("-"):
                key(call, w[:-1], w.endswith("+"))
            else:
                key(call, w, True)
                time.sleep(0.08)
                key(call, w, False)
            time.sleep(0.12)
    else:
        raise SystemExit(f"unknown command {cmd}")


if __name__ == "__main__":
    main()

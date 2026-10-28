# The README's desktop screenshots, from a release build (no debug banner) driven through tool/shots_main.dart.
#   uv run --with pyobjc-framework-Quartz python tool/shots.py [--no-build] [--box 10.0.0.18]
# Needs the box on Wi-Fi with daisy_synth on its TRS jacks (the play page draws the synth's controls from what the
# box read over MIDI-CI). Writes docs/images/desktop-*.png. Positions are logical pixels in a 1440 x 900 window.
# The phone's screenshots are taken by hand: adb exec-out screencap -p > docs/images/phone-<name>.png
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import Quartz

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).parent / "shots")]
from drive import Http  # noqa: E402
from project import project  # noqa: E402

APP = Path(__file__).parent.parent
OUT = APP.parent / "docs" / "images"
BUNDLE = APP / "build/macos/Build/Products/Release/airmidi.app"
URL = "http://127.0.0.1:47800/drive"


def window(pid):
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID):
        if w.get("kCGWindowOwnerPID") == pid and w["kCGWindowLayer"] == 0:
            return w["kCGWindowNumber"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--box", default="10.0.0.18")
    a = ap.parse_args()
    if not a.no_build:
        subprocess.run(["flutter", "build", "macos", "--release", "-t", "tool/shots_main.dart"], cwd=APP, check=True)
    app = subprocess.Popen([str(BUNDLE / "Contents/MacOS/airmidi"), "--window", "1440x900"])
    try:
        call = Http(URL)
        for _ in range(50):
            try:
                call("tap", {"x": -1, "y": -1})
                break
            except Exception:
                time.sleep(0.2)
        time.sleep(2)
        win = window(app.pid)
        OUT.mkdir(parents=True, exist_ok=True)

        def shot(name):
            time.sleep(1.5)
            subprocess.run(["screencapture", "-x", "-o", "-l", str(win), str(OUT / f"desktop-{name}.png")], check=True)
            print("desktop-" + name)

        def back():
            call("tap", {"x": 28, "y": 28})
            time.sleep(1.2)

        # the box's play page: the synth's controls (from its MIDI-CI list) over the keys
        call("tapText", {"text": "connect by address"})
        time.sleep(0.8)
        call("tapText", {"text": "10.0.0.18"})  # the address field's hint
        call("text", {"text": a.box})
        call("tapText", {"text": "connect"})
        time.sleep(5)
        call("tapText", {"text": "play", "n": 1})
        time.sleep(4)
        shot("play")
        back()
        back()

        # the arrangement: an MPE take's note pressure in the piano roll
        call("project", {"name": "demo song"}, json.dumps(project()).encode())
        call("tapText", {"text": "arrangement"})
        time.sleep(2)
        call("tap", {"x": 900, "y": 410})  # the take
        time.sleep(0.8)
        call("tap", {"x": 1044, "y": 711})  # its long note at bar 7
        time.sleep(0.5)
        call("tapText", {"text": "note pressure"})
        shot("arrangement")

        # the step sequencer: the pulse row's cutoff locked step by step
        call("tap", {"x": 900, "y": 330})  # the pluck pattern
        time.sleep(0.8)
        call("tap", {"x": 40, "y": 764})  # its pulse row, the lowest (row names are text fields)
        time.sleep(0.5)
        call("tapText", {"text": "Cutoff"})
        shot("sequencer")
    finally:
        app.terminate()


if __name__ == "__main__":
    main()

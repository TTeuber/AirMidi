# The project the screenshots show (lib/desktop/daw/project_file.dart's format): drums from the step sequencer, a
# pad and a sequenced pluck on daisy_synth (its MIDI-CI control list as the box read it, daisy_synth.json), and an
# MPE lead with each note's own bend and pressure. D minor, 96 bpm, eight bars.
import json
import math
from pathlib import Path

HERE = Path(__file__).parent


def scale_up(v, src, dst):
    """MIDI 1.0 -> 2.0 value scaling (M2-104 min-centre-max), as the app's up7to16 / up7to32."""
    shift = dst - src
    up = v << shift
    if v <= 1 << (src - 1):
        return up
    rb = src - 1
    rv = v & ((1 << rb) - 1)
    rv = rv << (shift - rb) if shift > rb else rv >> (rb - shift)
    while rv:
        up |= rv
        rv >>= rb
    return up


def vel(v):
    return scale_up(v, 7, 16)


def cc(v):
    return scale_up(v, 7, 32)


def daisy():
    return json.loads((HERE / "daisy_synth.json").read_text())


def drums(fill=False):
    rows = [(49, "crash"), (46, "open hat"), (42, "hat"), (39, "clap"), (38, "snare"), (36, "kick")]
    cells = {name: [] for _, name in rows}
    for s in range(16):
        if s in (6, 14):
            cells["open hat"].append({"i": s, "vel": vel(84), "gate": 0.8})
        elif s % 2 == 0:
            cells["hat"].append({"i": s, "vel": vel(100 if s % 4 == 0 else 72)})
        else:
            cells["hat"].append({"i": s, "vel": vel(46), "prob": 0.6})
    for s in (0, 7, 10):
        cells["kick"].append({"i": s, "vel": vel(118 if s == 0 else 96)})
    cells["snare"] += [{"i": 4, "vel": vel(110)}, {"i": 12, "vel": vel(110)}]
    cells["clap"] += [{"i": 12, "vel": vel(80), "nudge": 0.1}]
    if fill:
        cells["crash"].append({"i": 0, "vel": vel(110)})
        cells["snare"] += [{"i": 13, "vel": vel(70), "repeat": 2}, {"i": 15, "vel": vel(90), "repeat": 3}]
    return {"steps": 16, "stepBeats": 0.25, "swing": 0.56,
            "rows": [{"note": n, "name": name, "cells": cells[name]} for n, name in rows]}


def pad():
    # Dm9, Bbmaj7, Fmaj7/A, C6/9, two bars each; the cutoff opens over the eight bars and falls back
    chords = [[50, 57, 60, 64, 65], [46, 57, 62, 65, 69], [45, 57, 60, 64, 67], [48, 55, 62, 64, 69]]
    notes = []
    for i, ch in enumerate(chords):
        for j, p in enumerate(ch):
            notes.append({"t": i * 8 + j * 0.02, "len": 7.75, "pitch": p, "vel": vel(78 + 4 * j)})
    cutoff = [[0, cc(30), 1], [20, cc(96), 1], [28, cc(70), 1], [32, cc(40)]]  # 1: glide to the next point
    space = [[0, cc(70)], [24, cc(70), 1], [28, cc(110)], [32, cc(110)]]
    return {"length": 32, "notes": notes, "lanes": {"cc74": cutoff, "cc91": space}}


def pluck():
    # a D pulse on every eighth with the cutoff locked step by step (the low row), a pentatonic line over it
    rows = [(74, "d5"), (72, "c5"), (69, "a4"), (67, "g4"), (65, "f4"), (50, "d3")]
    line = {3: 69, 6: 72, 9: 74, 11: 67, 14: 65}
    cutoffs = [40, 58, 76, 100, 52, 70, 88, 118]
    cells = {n: [] for n, _ in rows}
    for k, s in enumerate(range(0, 16, 2)):
        cells[50].append({"i": s, "vel": vel(92 if k % 2 == 0 else 70), "gate": 0.3,
                          "locks": {"cc74": cc(cutoffs[k])}})
    for s, n in line.items():
        cells[n].append({"i": s, "vel": vel(64 + (s * 7) % 40), "gate": 0.6})
    return {"steps": 16, "stepBeats": 0.25,
            "rows": [{"note": n, "name": name, "cells": cells[n]} for n, name in rows]}


def lead():
    # an MPE take: every note its own bend (a slide into it, vibrato as it holds) and pressure
    line = [(0, 1.5, 69), (1.5, 0.5, 72), (2, 2, 74), (4.5, 1, 72), (5.5, 0.5, 69), (6, 2, 67),
            (8, 1, 65), (9, 1, 67), (10, 3, 69), (13.5, 0.5, 72), (14, 2, 74)]
    centre = 1 << 31
    notes = []
    for i, (t, ln, p) in enumerate(line):
        semitone = (1 << 31) // 48  # MPE bend range +-48
        bend = [[0, centre - 2 * semitone, 1], [0.12, centre]]
        if ln >= 2:
            bend += [[0.6 + k * 0.25, centre + (semitone if k % 2 else -semitone) // 2, 1]
                     for k in range(int((ln - 0.8) / 0.25))]
        peak = 70 + (i * 23) % 57
        pressure = [[0, cc(30), 1], [min(0.4, ln / 3), cc(peak), 1], [ln * 0.6, cc(peak - 25), 1], [ln, cc(20)]]
        notes.append({"t": t, "len": ln, "pitch": p, "vel": vel(90 + (i * 5) % 25),
                      "expr": {"bend": bend, "pressure": pressure, "timbre": [[0, cc(50)], [ln, cc(90), 1]]}})
    return {"length": 16, "notes": notes}


def project():
    dev = daisy()
    state = {f"cc{c['ctrlIndex'][0]}": c["default"] for c in dev["controls"] if c.get("ctrlIndex") and "default" in c}
    return {
        "format": "airmidi project", "version": 1, "bpm": 96.0, "beatsPerBar": 4,
        "loop": {"on": True, "start": 0.0, "end": 16.0}, "start": 0.0, "clockTo": [],
        "click": {"channel": 9, "note": 37}, "countIn": 1,
        "tracks": [
            {"id": 1, "name": "drums", "port": "IAC Driver Bus 1", "channel": 9, "clips": [
                {"id": 11, "name": "groove", "start": 0.0, "length": 16.0, "pattern": drums()},
                {"id": 12, "name": "groove fill", "start": 16.0, "length": 16.0, "pattern": drums(fill=True)}]},
            {"id": 2, "name": "pad", "port": "airmidi", "channel": 0, "device": dev, "program": 4, "state": state,
             "clips": [{"id": 21, "name": "chords", "start": 0.0, "length": 32.0, "midi": pad()}]},
            {"id": 3, "name": "pluck", "port": "airmidi", "channel": 1, "device": dev, "state": state, "clips": [
                {"id": 31, "name": "pluck seq", "start": 8.0, "length": 24.0, "pattern": pluck()}]},
            {"id": 4, "name": "lead (MPE)", "port": "Logic Pro Virtual In", "channel": 0, "mpe": True, "clips": [
                {"id": 41, "name": "take 3", "start": 16.0, "length": 16.0, "midi": lead()}]},
        ],
    }


if __name__ == "__main__":
    print(json.dumps(project(), indent=1))

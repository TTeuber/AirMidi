// Recording into MIDI clips (docs/daw.md "Recording"): an armed track's input, at the engine's timestamps, turned
// into notes and lanes. The take keeps what was played as it came and rebuilds the clip's content from it
// (the player asks every 20 ms), so earlier passes of a loop play back while the next is recorded, and a note
// still held shows as it grows.
//
// MPE (several notes on member channels, each bending on its own): each note keeps its channel's bend, pressure and
// timbre (CC74) while it is held, including what its channel sent just before the note on; the channel itself is
// not kept, playback hands out member channels afresh. A take is MPE when its track is, or when its notes come on
// three channels or more (a split keyboard sends two); then the track becomes MPE.
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/model.dart';

class Take {
  final Track track;
  final Clip clip;
  final bool grow; // no loop range: the clip grows with the take, a bar at a time
  final int beatsPerBar;
  final MidiContent _before; // the content as it was before the take
  final raw = <(double, List<int>)>[]; // song beats, as played
  double _last = 0;
  bool dirty = false;

  Take(this.track, this.clip, {required this.grow, this.beatsPerBar = 4})
      : _before = (clip.content as MidiContent).copy();

  /// A new take on [t] where recording starts at [startBeat]: into the MIDI clip that spans the loop range if
  /// there is one (overdub), else a new clip over the loop range; without a loop, a new clip from the bar.
  static Take start(Project p, Track t, double startBeat) {
    if (p.loopOn && p.loopEnd > p.loopStart) {
      for (final c in t.clips) {
        if (c.content is MidiContent && c.start <= p.loopStart + 1e-9 && c.end >= p.loopEnd - 1e-9) {
          return Take(t, c, grow: false, beatsPerBar: p.beatsPerBar);
        }
      }
      final len = p.loopEnd - p.loopStart;
      final c = Clip('take', p.loopStart, len, MidiContent(len));
      t.clips.add(c);
      return Take(t, c, grow: false, beatsPerBar: p.beatsPerBar);
    }
    final bar = (startBeat / p.beatsPerBar).floor() * p.beatsPerBar.toDouble();
    final c = Clip('take', math.max(0.0, bar), p.beatsPerBar.toDouble(), MidiContent(p.beatsPerBar.toDouble()));
    t.clips.add(c);
    return Take(t, c, grow: true, beatsPerBar: p.beatsPerBar);
  }

  void add(double beat, List<int> bytes) {
    if (bytes.isEmpty || bytes[0] >= 0xF0) return;
    raw.add((beat, bytes));
    _last = math.max(_last, beat);
    dirty = true;
  }

  /// Notes still held end at [beat] (play stopped).
  void finish(double beat) => flush(beat);

  /// The clip's content again: what it had before the take plus the take, notes held so far running to [now].
  void flush(double now) {
    final mpe = track.mpe || _channels() >= 3;
    if (mpe) track.mpe = true;
    if (grow) {
      final need = math.max(math.max(now, _last) - clip.start, 1e-6);
      final len = (need / beatsPerBar).ceil() * beatsPerBar.toDouble();
      clip.length = len;
      _before.length = len;
    }
    final out = _before.copy();
    final loopLen = out.length;
    double x(double beat) {
      final v = beat - clip.start + clip.offset;
      return grow ? v : v % loopLen;
    }

    final open = <(int, int), (Note, double)>{}; // (channel, pitch) -> the note and its song beat
    final current = <int, (Note, double)>{}; // MPE: a channel's sounding note
    final pending = <int, Map<String, int>>{}; // MPE: what a channel sent before its next note
    void lane(String name, double beat, int v) => out.lanes.putIfAbsent(name, () => []).add(Point(x(beat), v));
    for (final (beat, b) in raw) {
      final type = b[0] & 0xF0, ch = b[0] & 0x0F;
      final isOn = type == 0x90 && b.length > 2 && b[2] > 0;
      final isOff = type == 0x80 || (type == 0x90 && b.length > 2 && b[2] == 0);
      if (isOn) {
        final n = Note(x(beat), 0, b[1], velocity: up7to16(b[2]));
        if (mpe && ch != 0) {
          for (final e in (pending.remove(ch) ?? const {}).entries) {
            n.expression[e.key] = [Point(0, e.value)];
          }
          current[ch] = (n, beat);
        }
        open[(ch, b[1])] = (n, beat);
        out.notes.add(n);
        continue;
      }
      if (isOff) {
        final o = open.remove((ch, b[1]));
        if (o != null) {
          o.$1
            ..length = math.max(beat - o.$2, 1e-3)
            ..release = up7to16(b.length > 2 ? b[2] : 0);
          if (identical(current[ch]?.$1, o.$1)) current.remove(ch);
        }
        continue;
      }
      final String? name;
      final int v;
      if (type == 0xE0) {
        name = 'bend';
        v = up14to32(b[1] | b[2] << 7);
      } else if (type == 0xD0) {
        name = 'pressure';
        v = up7to32(b[1]);
      } else if (type == 0xB0) {
        name = mpe && ch != 0 && b[1] == 74 ? 'timbre' : 'cc${b[1]}';
        v = up7to32(b[2]);
      } else if (type == 0xC0) {
        name = 'program';
        v = up7to32(b[1]);
      } else {
        name = null;
        v = 0;
      }
      if (name == null) {
        out.other.add(RawEvent(x(beat), b));
      } else if (mpe && ch != 0 && (name == 'bend' || name == 'pressure' || name == 'timbre')) {
        final cur = current[ch];
        if (cur != null) {
          cur.$1.expression.putIfAbsent(name, () => []).add(Point(beat - cur.$2, v));
        } else {
          (pending[ch] ??= {})[name] = v;
        }
      } else {
        lane(name, beat, v);
      }
    }
    for (final (n, beat) in open.values) {
      n.length = math.max(now - beat, 1e-3);
    }
    out.notes.sort((a, b) => a.start.compareTo(b.start));
    for (final l in out.lanes.values) {
      _stableSort(l);
    }
    clip.content = out;
    dirty = false;
  }

  int _channels() => {
        for (final (_, b) in raw)
          if ((b[0] & 0xF0) == 0x90 && b.length > 2 && b[2] > 0) b[0] & 0x0F
      }.length;
}

void _stableSort(List<Point> l) {
  final idx = {for (var i = 0; i < l.length; i++) l[i]: i};
  l.sort((a, b) => a.t != b.t ? a.t.compareTo(b.t) : idx[a]!.compareTo(idx[b]!));
}

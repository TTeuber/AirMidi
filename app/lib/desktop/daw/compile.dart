// The arrangement as timed MIDI (docs/daw.md "Playback"): every clip that overlaps a stretch of song time gives its
// events, each note on with its note off (cut at the clip's end, at the end of each pass through looping content,
// and at the loop's end), lanes and expression points where they fall. The player asks for the next ~120 ms every
// 20 ms, so every event belongs to exactly one window: windows join without gaps or doubles.
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/time.dart';

/// MPE member channels (1..15, the lower zone) for [notes], handed out afresh: a free channel, the one that has
/// been free longest; when none is free, the one whose note ends first. The same notes always get the same channels,
/// so a note's expression in a later window goes where its note on went.
Map<Note, int> allocateMembers(List<Note> notes, {int members = 15}) {
  final order = [...notes.asMap().entries]
    ..sort((a, b) => a.value.start != b.value.start ? a.value.start.compareTo(b.value.start) : a.key.compareTo(b.key));
  final busyUntil = List<double>.filled(members, double.negativeInfinity);
  final out = <Note, int>{};
  for (final e in order) {
    final n = e.value;
    var best = -1;
    for (var c = 0; c < members; c++) {
      if (busyUntil[c] <= n.start && (best < 0 || busyUntil[c] < busyUntil[best])) best = c;
    }
    if (best < 0) {
      best = 0;
      for (var c = 1; c < members; c++) {
        if (busyUntil[c] < busyUntil[best]) best = c;
      }
    }
    busyUntil[best] = n.end;
    out[n] = best + 1;
  }
  return out;
}

// A number in [0, 1) from the step's identity: the same hit always rolls the same, wherever the windows fall.
double _roll(int a, int b, int c, int d) {
  var h = 0x811C9DC5;
  for (final x in [a, b, c, d]) {
    h = ((h ^ (x & 0xFFFFFFFF)) * 0x01000193) & 0xFFFFFFFF;
    h ^= h >> 15;
  }
  return h / 0x100000000;
}

List<int> _lane(String name, int ch, int v) {
  if (name == 'bend') {
    final b = down32to14(v);
    return [0xE0 | ch, b & 0x7F, b >> 7];
  }
  if (name == 'pressure') return [0xD0 | ch, down32to7(v)];
  if (name == 'program') return [0xC0 | ch, down32to7(v)];
  if (name.startsWith('cc')) return [0xB0 | ch, int.parse(name.substring(2)), down32to7(v)];
  return const [];
}

List<int> _expression(String name, int ch, int v) => switch (name) {
      'bend' => _lane('bend', ch, v),
      'pressure' => _lane('pressure', ch, v),
      'timbre' => [0xB0 | ch, 74, down32to7(v)],
      _ => const [],
    };

/// One hit of a pattern, in its content's time: a step, or one of a step's repeats.
class PatternHit {
  final double x, length; // beats; x can be a little below 0 (a nudged first step) or past the end
  final int row, step, hit, pitch, velocity; // velocity 7 bit
  final double probability;
  const PatternHit(this.x, this.length, this.row, this.step, this.hit, this.pitch, this.velocity, this.probability);

  /// Whether this hit sounds in [pass] of the clip [clipId]: always at probability 1, else a roll that is the same
  /// every time for the same hit and pass.
  bool plays(int clipId, int pass) =>
      probability >= 1 || _roll(clipId, pass, row * kMaxSteps + step, hit) < probability;
}

/// Every hit of [p] (unmuted rows), with swing, nudge, repeats and gate applied: what the compiler plays each pass
/// and what "to MIDI clip" writes.
Iterable<PatternHit> patternHits(Pattern p) sync* {
  final sb = p.stepBeats;
  for (var r = 0; r < p.rows.length; r++) {
    final row = p.rows[r];
    if (row.mute) continue;
    for (var s = 0; s < p.steps; s++) {
      final st = p.cells[r][s];
      if (st == null) continue;
      final x = s * sb + (s.isOdd ? (p.swing - 0.5) * 2 * sb : 0) + st.nudge * sb;
      final hits = st.repeat.clamp(1, 4), each = sb / hits;
      final len = math.min(st.gate * sb, hits > 1 ? each * 0.9 : double.infinity);
      for (var h = 0; h < hits; h++) {
        yield PatternHit(x + h * each, len, r, s, h, row.note, down16to7(st.velocity), st.probability);
      }
    }
  }
}

/// A pattern as notes ("to MIDI clip"): one pass of it, the probabilities rolled as the clip's first pass plays
/// them; a hit nudged before the start wraps to the end.
MidiContent patternToMidi(Pattern p, int clipId) {
  final len = p.length;
  return MidiContent(len,
      notes: [
        for (final h in patternHits(p))
          if (h.plays(clipId, 0)) Note(h.x < 0 ? h.x + len : h.x, h.length, h.pitch, velocity: up7to16(h.velocity))
      ]..sort((a, b) => a.start.compareTo(b.start)));
}

/// The events in song beats [from, to), timed by [nsAt]. Notes end by [cut] at the latest (the loop's end when the
/// stretch ends there). [pass] tells repeats of the song loop apart (each pass rolls its own probabilities).
/// [notes] collects the notes started, for stopping.
List<SeqEvent> compile(Project p, double from, double to, int Function(double beat) nsAt,
    {double cut = double.infinity, int pass = 0, List<ScheduledNote>? notes}) {
  final out = <SeqEvent>[];
  for (final t in p.audible) {
    final port = t.port;
    if (port == null) continue;
    for (final c in t.clips) {
      if (c.end <= from || c.start >= to) continue;
      final len = c.content.length;
      if (len <= 0) continue;
      final limit = math.min(c.end, cut);
      // the passes through the content that can have events in the window (one either side for nudged steps)
      final k0 = c.loops ? ((math.max(from, c.start) - c.start + c.offset) / len).floor() - 1 : 0;
      final k1 = c.loops ? ((math.min(to, c.end) - c.start + c.offset) / len).floor() + 1 : 0;
      for (var k = math.max(0, k0); k <= k1; k++) {
        final base = c.start - c.offset + k * len, passEnd = math.min(base + len, limit);
        if (base >= limit) break;
        bool inWindow(double at) => at >= from && at < to && at >= c.start && at < limit;
        void note(double at, double off, int ch, int pitch, int vel,
            {List<int>? before, double? end, int release = 0}) {
          off = math.min(off, end ?? passEnd); // a nudged step may end past its pass, never past the clip
          if (off <= at) return;
          final on = nsAt(at), offNs = nsAt(off);
          if (before != null) out.add(SeqEvent(on, port, before));
          out.add(SeqEvent(on, port, [0x90 | ch, pitch, vel.clamp(1, 127)]));
          out.add(SeqEvent(offNs, port, [0x80 | ch, pitch, release]));
          notes?.add(ScheduledNote(port, ch, pitch, offNs));
        }

        final content = c.content;
        if (content is MidiContent) {
          final members = t.mpe ? allocateMembers(content.notes) : const <Note, int>{};
          final main = t.mpe ? 0 : t.channel;
          for (final n in content.notes) {
            final at = base + n.start;
            if (at < c.start || at >= passEnd) continue;
            final ch = members[n] ?? main;
            final off = math.min(at + n.length, passEnd);
            if (inWindow(at)) {
              // an MPE note's channel starts from its own bend (or the centre): the last note there may have bent it
              final bend = n.expression['bend'];
              final first = t.mpe && (bend == null || bend.isEmpty || bend.first.t > 1e-9)
                  ? _expression('bend', ch, kCentre32)
                  : null;
              note(at, off, ch, n.pitch, down16to7(n.velocity), before: first, release: down16to7(n.release));
            }
            if (!t.mpe) continue;
            for (final e in n.expression.entries) {
              for (final pt in e.value) {
                final pa = at + math.max(0.0, pt.t);
                if (pa < off && inWindow(pa)) out.add(SeqEvent(nsAt(pa), port, _expression(e.key, ch, pt.v)));
              }
            }
          }
          for (final l in content.lanes.entries) {
            for (final pt in l.value) {
              final at = base + pt.t;
              if (at < passEnd && inWindow(at)) {
                final m = _lane(l.key, main, pt.v);
                if (m.isNotEmpty) out.add(SeqEvent(nsAt(at), port, m));
              }
            }
          }
          for (final r in content.other) {
            final at = base + r.t;
            if (at < passEnd && inWindow(at)) out.add(SeqEvent(nsAt(at), port, r.bytes));
          }
        } else if (content is Pattern) {
          final ch = t.mpe ? 0 : t.channel;
          for (final h in patternHits(content)) {
            final at = base + h.x;
            if (!inWindow(at) || !h.plays(c.id, pass * 100003 + k)) continue;
            note(at, at + h.length, ch, h.pitch, h.velocity, end: limit);
          }
        }
      }
    }
  }
  return out;
}

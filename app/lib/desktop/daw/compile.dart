// The arrangement as timed MIDI (docs/daw.md "Playback"): every clip that overlaps a stretch of song time gives its
// events, each note on with its note off (cut at the clip's end, at the end of each pass through looping content,
// and at the loop's end), lanes and expression points where they fall. The player asks for the next ~120 ms every
// 20 ms, so every event belongs to exactly one window: windows join without gaps or doubles.
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/lanes.dart' show lockBase;
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

/// How finely a ramp plays: a value every 1/64 beat at most (8 ms at 120 bpm), and only when the message changes.
const kRampStep = 1 / 64;

/// The values [pts] sends in [from, to) (times in the points' own frame): each point, and along a ramp the grid
/// steps where [message] changes. The same values come out however the windows fall.
Iterable<(double, int)> laneValues(List<Point> pts, double from, double to, List<int> Function(int) message) sync* {
  for (var i = 0; i < pts.length; i++) {
    final p = pts[i];
    if (p.t >= from && p.t < to) yield (p.t, p.v);
    if (!p.ramp || i + 1 >= pts.length) continue;
    final q = pts[i + 1], span = q.t - p.t;
    if (span <= 0 || q.t <= from || p.t >= to) continue;
    int at(int k) => p.v + ((q.v - p.v) * (k * kRampStep / span)).round();
    final n = (span / kRampStep).ceil();
    for (var k = math.max(1, ((from - p.t) / kRampStep).floor()); k < n; k++) {
      final t = p.t + k * kRampStep;
      if (t >= to) break;
      if (t < from) continue;
      final v = at(k);
      if (!_same(message(v), message(at(k - 1)))) yield (t, v);
    }
  }
}

bool _same(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

List<int> _expression(String name, int ch, int v) => switch (name) {
      'bend' => _lane('bend', ch, v),
      'pressure' => _lane('pressure', ch, v),
      'timbre' => [0xB0 | ch, 74, down32to7(v)],
      _ => const [],
    };

/// Controller chase (docs/daw.md "Playback"): the last value of every controller, bend, channel pressure and program
/// before song beat [at], per port and channel, all timed [ns]. Sent first when play starts in the middle (the
/// synth's filter is where the song has it, not where it was left) and where the loop starts again. MPE notes' own
/// expression is not chased: it belongs to notes that are not sounding.
List<SeqEvent> chase(Project p, double at, int ns) {
  if (at <= 0) return const [];
  final ev = compile(p, 0, at, (b) => (b * 1e6).round());
  sortEvents(ev);
  final last = <String, SeqEvent>{};
  final mpePorts = {
    for (final t in p.audible)
      if (t.mpe && t.port != null) t.port!
  };
  for (final e in ev) {
    final type = e.bytes[0] & 0xF0, ch = e.bytes[0] & 0x0F;
    if (type != 0xB0 && type != 0xE0 && type != 0xD0 && type != 0xC0) continue;
    if (mpePorts.contains(e.port) && ch != 0) continue;
    last['${e.port} $type $ch ${type == 0xB0 ? e.bytes[1] : ''}'] = e;
  }
  return [for (final e in last.values) SeqEvent(ns, e.port, e.bytes)];
}

/// What a track's synth is set to before anything plays (docs/daw.md "Projects and files"): bank select, program change,
/// then the synth's saved settings, per track that sends somewhere (muted ones too: they set the synth, they do not
/// sound), all timed [ns]. Sent when the project opens and when play starts (before the chase, so the song's own
/// lanes have the last word).
List<SeqEvent> setupEvents(Project p, int ns, {Iterable<Track>? tracks}) {
  final out = <SeqEvent>[];
  for (final t in tracks ?? p.tracks) {
    final port = t.port;
    if (port == null) continue;
    final ch = t.mainChannel;
    if (t.program != null) {
      if (t.bankMsb != null) out.add(SeqEvent(ns, port, [0xB0 | ch, 0, t.bankMsb!]));
      if (t.bankLsb != null) out.add(SeqEvent(ns, port, [0xB0 | ch, 32, t.bankLsb!]));
      out.add(SeqEvent(ns, port, [0xC0 | ch, t.program!]));
    }
    final keys = t.state.keys.toList()..sort();
    for (final k in keys) {
      final m = _lane(k, ch, t.state[k]!);
      if (m.isNotEmpty) out.add(SeqEvent(ns, port, m));
    }
  }
  return out;
}

/// The controllers [p]'s steps lock, in a fixed order.
List<String> lockKeys(Pattern p) {
  final keys = <String>{};
  for (var r = 0; r < p.rows.length; r++) {
    if (p.rows[r].mute) continue;
    for (var s = 0; s < p.steps; s++) {
      keys.addAll(p.cells[r][s]?.locks.keys ?? const []);
    }
  }
  return keys.toList()..sort();
}

/// Parameter locks as values in time (content beats): before every group of hits that play together, each locked
/// controller's value, the first locked step's in the group (by row), else [base]'s (the track's own value: the lock
/// lasts one step); a controller with no base keeps its lock until the next. Every group sends, so a step that does
/// not play (probability) never leaves its lock behind.
Iterable<(double, String, int)> lockValues(Pattern p, Iterable<PatternHit> hits, int? Function(String) base) sync* {
  final keys = lockKeys(p);
  if (keys.isEmpty) return;
  final groups = <double, List<PatternHit>>{};
  for (final h in hits) {
    groups.putIfAbsent(h.x, () => []).add(h);
  }
  for (final g in groups.entries) {
    for (final k in keys) {
      int? v;
      for (final h in g.value) {
        v ??= p.cells[h.row][h.step]?.locks[k];
      }
      v ??= base(k);
      if (v != null) yield (g.key, k, v);
    }
  }
}

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

/// A pattern as notes ("to MIDI clip"): one pass of it, the probabilities rolled as its first pass plays them; a
/// hit nudged before the start wraps to the end. Its locks become lanes, a point before each step as it plays them
/// ([base]: the track's own values).
MidiContent patternToMidi(Pattern p, int clipId, {int? Function(String)? base}) {
  final len = p.length;
  double wrap(double x) => x < 0 ? x + len : x;
  final hits = [
    for (final h in patternHits(p))
      if (h.plays(clipId, 0)) h
  ];
  final lanes = <String, List<Point>>{};
  for (final (x, k, v) in lockValues(p, hits, base ?? (_) => null)) {
    lanes.putIfAbsent(k, () => []).add(Point(wrap(x), v));
  }
  for (final l in lanes.values) {
    l.sort((a, b) => a.t.compareTo(b.t));
  }
  return MidiContent(len,
      notes: [for (final h in hits) Note(wrap(h.x), h.length, h.pitch, velocity: up7to16(h.velocity))]
        ..sort((a, b) => a.start.compareTo(b.start)),
      lanes: lanes);
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
              // points before the note on (sent just before it) play at its start
              final pts = [for (final pt in e.value) pt.t < 0 ? Point(0, pt.v, ramp: pt.ramp) : pt];
              List<int> msg(int v) => _expression(e.key, ch, v);
              for (final (t, v) in laneValues(pts, from - at, math.min(to, off) - at, msg)) {
                final pa = at + t;
                if (pa < off && inWindow(pa)) out.add(SeqEvent(nsAt(pa), port, msg(v)));
              }
            }
          }
          for (final l in content.lanes.entries) {
            List<int> msg(int v) => _lane(l.key, main, v);
            for (final (t, v) in laneValues(l.value, from - base, math.min(to, passEnd) - base, msg)) {
              final at = base + t;
              if (at < passEnd && inWindow(at)) {
                final m = msg(v);
                if (m.isNotEmpty) out.add(SeqEvent(nsAt(at), port, m));
              }
            }
          }
          for (final r in content.other) {
            final at = base + r.t;
            if (at < passEnd && inWindow(at)) out.add(SeqEvent(nsAt(at), port, r.bytes));
          }
        } else if (content is Pattern) {
          final ch = t.mainChannel;
          final hits = [
            for (final h in patternHits(content))
              if (inWindow(base + h.x) && h.plays(c.id, pass * 100003 + k)) h
          ];
          // locks just before their notes (sortEvents puts controllers before note ons at the same time)
          for (final (x, key, v) in lockValues(content, hits, (key) => lockBase(t, key))) {
            final m = _lane(key, ch, v);
            if (m.isNotEmpty) out.add(SeqEvent(nsAt(base + x), port, m));
          }
          for (final h in hits) {
            final at = base + h.x;
            note(at, at + h.length, ch, h.pitch, h.velocity, end: limit);
          }
        }
      }
    }
  }
  return out;
}

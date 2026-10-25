// Strip geometry and touch logic, ported from modeling/proto/pedal_steel/strips.js. No widgets, no MIDI:
// positions in, pitch events out. Pitch is a float MIDI note; the centre of a row is exactly in tune.
import 'dart:math' as math;

/// Flutter logical pixels per mm. Android's density-independent pixel is nominally 1/160 inch; the Pixel 9a's
/// real density is within a few percent of that.
const double kPxPerMm = 160 / 25.4;

const kNames = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b'];

// Guitar strings in the order strips get added: D G B first, then high E, A, low E.
const _addOrder = [50, 55, 59, 64, 45, 40];
List<int> defaultTuning(int n) => (_addOrder.take(n).toList()..sort());

/// Fret markers like a guitar neck, by row below the open note: one mark, two at the octaves.
const kFretMarkers = {3: 1, 5: 1, 7: 1, 9: 1, 12: 2, 15: 1, 17: 1, 19: 1, 21: 1, 24: 2};
const _markerCol = 6.0; // mm, the marker column left of the first strip

int _pc(int m) => ((m % 12) + 12) % 12;
String noteName(int m) => kNames[_pc(m)] + ((m / 12).floor() - 1).toString();

/// Key guide: scales as semitones above the root.
const kScales = {
  'maj': [0, 2, 4, 5, 7, 9, 11],
  'min': [0, 2, 3, 5, 7, 8, 10],
  'pent': [0, 2, 4, 7, 9],
};
bool inKey(int note, int root, String scale) => !kScales.containsKey(scale) || kScales[scale]!.contains(_pc(note - root));
bool isRoot(int note, int root) => _pc(note - root) == 0;

/// Pitch assist, fully applied: a soft quantizer. e = distance from the nearest note; the remaining error is
/// 0.5 * |2e|^k, flat around each note and continuous at the row boundaries, so a slide still passes every pitch.
/// k = 1 changes nothing; k = 12 (full assist) leaves 0.1 cents of a 30-cent miss and 3 cents of a 40-cent one.
double soften(double x, double k) {
  final n = x.roundToDouble(), e = x - n;
  return n + e.sign * 0.5 * math.pow(2 * e.abs(), k);
}

const _releaseSpeed = 3.0; // semitones/s: at this finger speed the assist has let go completely
const _speedTau = 0.04; // s, smoothing of the finger speed
const _letGoTau = 0.025; // s, how fast the assist lets go when the finger starts moving

/// Nearest note and the deviation from it in cents (-50..50).
({int note, int cents}) nearest(double pitch) {
  final note = pitch.round();
  return (note: note, cents: ((pitch - note) * 100).round());
}

enum StripAlign { left, mid, right }

class LayoutSettings {
  final double row, width, gap; // mm
  final StripAlign align;
  final List<int> tuning;
  const LayoutSettings({required this.row, required this.width, required this.gap, required this.align, required this.tuning});
}

class Strip {
  final int i, base;
  final double x, w;
  const Strip(this.i, this.base, this.x, this.w);
}

class StripLayout {
  final List<Strip> strips;
  final double top, bottom, row, gap, markerX;
  const StripLayout(this.strips, this.top, this.bottom, this.row, this.gap, this.markerX);

  double pitchAt(Strip s, double y) => s.base + (y - top) / row - 0.5;
  double yAt(Strip s, double pitch) => top + (pitch - s.base + 0.5) * row;

  /// The strip a touch at x belongs to: inside a strip, or within half a gap of one (and the same past the outer
  /// edges).
  Strip? stripAt(double x, [double pxPerMm = kPxPerMm]) {
    final reach = math.max(gap / 2, 4 * pxPerMm);
    for (final s in strips) {
      final left = s.i == 0 ? reach : gap / 2, right = s.i == strips.length - 1 ? reach : gap / 2;
      if (x >= s.x - left && x < s.x + s.w + right) return s;
    }
    return null;
  }
}

/// field: (x, y, w, h) in px. Returns the strips' columns and the row height in px.
StripLayout layout(double fx, double fy, double fw, double fh, LayoutSettings s, [double pxPerMm = kPxPerMm]) {
  final w = s.width * pxPerMm, gap = s.gap * pxPerMm, margin = 8 * pxPerMm;
  final markerGap = math.max(gap, 2 * pxPerMm), col = _markerCol * pxPerMm;
  final n = s.tuning.length, total = n * w + (n - 1) * gap;
  final x0 = switch (s.align) {
    StripAlign.left => fx + margin + col + markerGap,
    StripAlign.right => fx + fw - margin - total,
    StripAlign.mid => fx + (fw - total) / 2,
  };
  final strips = [for (var i = 0; i < n; i++) Strip(i, s.tuning[i], x0 + i * (w + gap), w)];
  final markerX = x0 - markerGap - col / 2; // centre of the marker column
  return StripLayout(strips, fy, fy + fh, s.row * pxPerMm, gap, markerX);
}

enum EventType { on, change, pitch, off }

/// 'change' = the sounding touch on a strip switched (hammer-on / pull-off).
class StripEvent {
  final EventType type;
  final int strip;
  final double pitch; // NaN for off
  const StripEvent(this.type, this.strip, [this.pitch = double.nan]);

  @override
  bool operator ==(Object other) =>
      other is StripEvent && other.type == type && other.strip == strip &&
      (other.pitch == pitch || (other.pitch.isNaN && pitch.isNaN));
  @override
  int get hashCode => Object.hash(type, strip);
  @override
  String toString() => 'StripEvent(${type.name}, $strip, $pitch)';
}

class _Touch {
  final int strip;
  double raw, offset, last, speed = 0, w = 0;
  _Touch(this.strip, this.raw, this.offset) : last = raw;
}

class Assist {
  final double amount; // 0..1
  final double pull; // time constant (s) of pulling into tune
  const Assist(this.amount, this.pull);
}

/// One note per strip. Every touch keeps the strip it landed on until it lifts; the newest touch on a strip
/// sounds, and lifting it falls back to the one under it (hammer-on / pull-off).
class Strings {
  final _touches = <int, _Touch>{}; // pointer id -> touch
  final _stacks = <int, List<int>>{}; // strip -> ids, last = sounding
  Assist assist = const Assist(0, 0.15);

  int get touchCount => _touches.length;
  int? stripOf(int id) => _touches[id]?.strip;

  /// Sounding pitch: the finger's pitch (+ snap offset), pulled toward the nearest note by the assist weight w,
  /// which rises while the finger rests and falls while it moves.
  double pitchOf(int id) {
    final t = _touches[id]!, x = t.raw + t.offset;
    if (assist.amount == 0 || t.w == 0) return x;
    return x + t.w * (soften(x, 1 + 11 * assist.amount) - x);
  }

  /// Advance the assist by dt seconds (call every frame while touches are down). Returns pitch events for the
  /// sounding touches whose pitch moved.
  List<StripEvent> tick(double dt) {
    final events = <StripEvent>[];
    _stacks.forEach((strip, stack) {
      for (final id in stack) {
        final t = _touches[id]!, before = pitchOf(id);
        final inst = (t.raw - t.last).abs() / dt;
        t.last = t.raw;
        t.speed += (inst - t.speed) * (1 - math.exp(-dt / _speedTau));
        final target = assist.amount > 0 ? math.max(0.0, 1 - t.speed / _releaseSpeed) : 0.0;
        final tau = target > t.w ? assist.pull : _letGoTau;
        t.w += (target - t.w) * (1 - math.exp(-dt / tau));
        final after = pitchOf(id);
        if (id == stack.last && (after - before).abs() > 1e-4) events.add(StripEvent(EventType.pitch, strip, after));
      }
    });
    return events;
  }

  /// snap: the touch starts on the nearest note wherever it lands in the row, then moves continuously from there.
  StripEvent down(int id, int strip, double raw, bool snap) {
    _touches[id] = _Touch(strip, raw, snap ? raw.roundToDouble() - raw : 0);
    final stack = _stacks.putIfAbsent(strip, () => []);
    stack.add(id);
    return StripEvent(stack.length == 1 ? EventType.on : EventType.change, strip, pitchOf(id));
  }

  StripEvent? move(int id, double raw) {
    final t = _touches[id];
    if (t == null) return null;
    t.raw = raw;
    return _stacks[t.strip]!.last == id ? StripEvent(EventType.pitch, t.strip, pitchOf(id)) : null;
  }

  StripEvent? up(int id) {
    final t = _touches[id];
    if (t == null) return null;
    final stack = _stacks[t.strip]!;
    final wasTop = stack.last == id;
    stack.remove(id);
    _touches.remove(id);
    if (stack.isEmpty) {
      _stacks.remove(t.strip);
      return StripEvent(EventType.off, t.strip);
    }
    return wasTop ? StripEvent(EventType.change, t.strip, pitchOf(stack.last)) : null;
  }

  /// For drawing: the sounding pitch and the held-but-silent pitches of a strip.
  ({double pitch, List<double> held})? state(int strip) {
    final stack = _stacks[strip];
    if (stack == null) return null;
    return (pitch: pitchOf(stack.last), held: [for (final id in stack.take(stack.length - 1)) pitchOf(id)]);
  }

  List<StripEvent> clear() {
    final offs = [for (final s in _stacks.keys) StripEvent(EventType.off, s)];
    _touches.clear();
    _stacks.clear();
    return offs;
  }
}

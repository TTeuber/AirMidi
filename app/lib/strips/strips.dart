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

const _edge = 1.0; // mm left free at either side of the screen

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

class LayoutSettings {
  final double row, gap; // mm
  final double? width; // mm; null = fit the strips to the screen
  final List<int> tuning;
  final double view; // semitones: the pitch at the top edge, above the open strings (the hand up the neck)
  final double scroll; // mm, how far wide strips are scrolled to the left
  const LayoutSettings(
      {required this.row, this.width, required this.gap, required this.tuning, this.view = 0, this.scroll = 0});

  LayoutSettings copyWith({double? row, double? Function()? width, double? view, double? scroll}) => LayoutSettings(
      row: row ?? this.row,
      width: width != null ? width() : this.width,
      gap: gap,
      tuning: tuning,
      view: view ?? this.view,
      scroll: scroll ?? this.scroll);
}

class Strip {
  final int i, base;
  final double x, w;
  const Strip(this.i, this.base, this.x, this.w);
}

/// Every length in px. Strips wider than their area scroll.
class StripLayout {
  final List<Strip> strips;
  final double top, bottom, row, gap;
  final double areaL, areaR; // the strips' area; strips outside it are clipped
  final double fitWidth; // the strip width that fills the area exactly
  final double view, pxPerMm;
  const StripLayout(this.strips, this.top, this.bottom, this.row, this.gap,
      {required this.areaL, required this.areaR, required this.fitWidth, this.view = 0, this.pxPerMm = kPxPerMm});

  double pitchAt(Strip s, double y) => s.base + view + (y - top) / row - 0.5;
  double yAt(Strip s, double pitch) => top + (pitch - s.base - view + 0.5) * row;

  double get total => strips.isEmpty ? 0 : strips.last.x + strips.last.w - strips.first.x;
  bool get overflows => total > areaR - areaL + 0.5;

  /// The strip a touch at x belongs to: inside a strip, or within half a gap of one (and the same past the outer
  /// edges). With scrolled strips, a touch outside the area plays the strip at its edge, never a hidden one.
  Strip? stripAt(double x, [double? pxPerMm]) {
    if (overflows) x = x.clamp(areaL, areaR - 0.01);
    final reach = math.max(gap / 2, 4 * (pxPerMm ?? this.pxPerMm));
    for (final s in strips) {
      final left = s.i == 0 ? reach : gap / 2, right = s.i == strips.length - 1 ? reach : gap / 2;
      if (x >= s.x - left && x < s.x + s.w + right) return s;
    }
    return null;
  }
}

/// field: (x, y, w, h) in px. Strips that fit sit centred in the area; wider ones start at its left edge, scrolled.
StripLayout layout(double fx, double fy, double fw, double fh, LayoutSettings s, [double pxPerMm = kPxPerMm]) {
  final gap = s.gap * pxPerMm, edge = _edge * pxPerMm;
  final areaL = fx + edge, areaR = fx + fw - edge, areaW = areaR - areaL;
  final n = s.tuning.length;
  final fit = n == 0 ? 0.0 : (areaW - (n - 1) * gap) / n;
  final w = s.width == null ? fit : s.width! * pxPerMm, total = n * w + (n - 1) * gap;
  final x0 = total <= areaW
      ? areaL + (areaW - total) / 2
      : areaL - (s.scroll * pxPerMm).clamp(0.0, total - areaW);
  final strips = [for (var i = 0; i < n; i++) Strip(i, s.tuning[i], x0 + i * (w + gap), w)];
  return StripLayout(strips, fy, fy + fh, s.row * pxPerMm, gap,
      areaL: areaL, areaR: areaR, fitWidth: fit, view: s.view, pxPerMm: pxPerMm);
}

/// Move mode's fingers: one drags, two pinch. Each step compares the fingers with where they were on the previous
/// event: the focal point (their centre) before and after, and the spread's change along each axis. An axis whose
/// spread is under minSpan does not scale, so fingers side by side resize the strips without zooming the rows.
class PinchStep {
  final double fx, fy, nx, ny, sx, sy;
  const PinchStep(this.fx, this.fy, this.nx, this.ny, this.sx, this.sy);
}

class Pinch {
  final double minSpan; // px
  Pinch(this.minSpan);
  final _at = <int, (double, double)>{};
  ({double cx, double cy, double spanX, double spanY})? _was;

  int get count => _at.length;

  ({double cx, double cy, double spanX, double spanY}) _stats() {
    var cx = 0.0, cy = 0.0;
    for (final (x, y) in _at.values) {
      cx += x / _at.length;
      cy += y / _at.length;
    }
    var sx = 0.0, sy = 0.0; // twice the mean distance from the centre: for two fingers, their distance on the axis
    for (final (x, y) in _at.values) {
      sx += 2 * (x - cx).abs() / _at.length;
      sy += 2 * (y - cy).abs() / _at.length;
    }
    return (cx: cx, cy: cy, spanX: sx, spanY: sy);
  }

  // A finger landing or lifting moves the centre without anything having moved: start the comparison again.
  void down(int id, double x, double y) {
    _at[id] = (x, y);
    _was = _stats();
  }

  void up(int id) {
    _at.remove(id);
    _was = _at.isEmpty ? null : _stats();
  }

  void clear() {
    _at.clear();
    _was = null;
  }

  PinchStep? move(int id, double x, double y) {
    final a = _was;
    if (a == null || !_at.containsKey(id)) return null;
    _at[id] = (x, y);
    final b = _was = _stats();
    double scale(double s0, double s1) => s0 >= minSpan && s1 >= minSpan ? s1 / s0 : 1;
    return PinchStep(a.cx, a.cy, b.cx, b.cy, scale(a.spanX, b.spanX), scale(a.spanY, b.spanY));
  }
}

const kRowRange = (6.0, 30.0), kWidthRange = (8.0, 40.0); // mm

/// One move-mode step applied to the view: the pitch and the strip under the focal point follow it, and rows and
/// strips grow or shrink around it. The view stays where every visible row is a MIDI note.
LayoutSettings moved(StripLayout L, LayoutSettings s, PinchStep p) {
  final mm = L.pxPerMm;
  // rows: the pitch under the focal point before = the pitch under it after
  final row = (s.row * p.sy).clamp(kRowRange.$1, kRowRange.$2), rowPx = row * mm;
  final view = s.view + (p.fy - L.top) / L.row - (p.ny - L.top) / rowPx;
  final lo = -s.tuning.reduce(math.min).toDouble();
  final hi = math.max(lo, 127 - s.tuning.reduce(math.max) - (L.bottom - L.top) / rowPx + 0.5);
  // strips: the same for the position across the strips (in strip widths from the first strip's left edge)
  final w0 = L.strips.first.w, unit0 = w0 + L.gap;
  final width = p.sx == 1 ? s.width : (w0 / mm * p.sx).clamp(kWidthRange.$1, kWidthRange.$2);
  final w1 = width == null ? L.fitWidth : width * mm, unit1 = w1 + L.gap;
  final x0 = p.nx - (p.fx - L.strips.first.x) / unit0 * unit1;
  final total = L.strips.length * w1 + (L.strips.length - 1) * L.gap, areaW = L.areaR - L.areaL;
  final scroll = total <= areaW ? 0.0 : (L.areaL - x0).clamp(0.0, total - areaW) / mm;
  return s.copyWith(row: row, width: () => width, view: view.clamp(lo, hi), scroll: scroll);
}

/// Where the fingers lift in move mode: rows back on whole semitones.
LayoutSettings settled(LayoutSettings s) => s.copyWith(view: s.view.roundToDouble());

/// Where move mode ends: a whole number of strips fills the screen, no slivers at the edges. The count is the one
/// nearest to what is on screen now (all of them = "fit"), within the width range; the scroll lands on a strip.
LayoutSettings quantized(StripLayout L, LayoutSettings s) {
  final n = L.strips.length, areaW = L.areaR - L.areaL, mm = L.pxPerMm;
  if (n == 0) return s;
  double widthFor(int k) => (areaW - (k - 1) * L.gap) / k;
  var k = (areaW / (L.strips.first.w + L.gap)).round().clamp(1, n);
  while (k < n && widthFor(k) / mm > kWidthRange.$2) {
    k++;
  }
  if (k == n) return s.copyWith(width: () => null, scroll: 0);
  final w = widthFor(k), unit = w + L.gap;
  final first = ((s.scroll * mm) / (L.strips.first.w + L.gap)).round().clamp(0, n - k); // leftmost strip on screen
  return s.copyWith(width: () => w / mm, scroll: first * unit / mm);
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

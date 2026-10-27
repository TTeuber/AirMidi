// The synth page's layout from the control paths (M2-117 paramPath; conventions in docs/system/README.md, "Control
// paths"): one section per first path segment, in the order of the best control in it, and widgets for the groups
// an editor recognises: attack, decay, sustain and release under one parent path become an envelope, cutoff and
// resonance an XY pad. A device that sends no paths gets the same from its titles ("Amp Attack", "Amp Decay", ...).
import 'dart:math' as math;

import 'package:airmidi/shared/synth/controls.dart';

class Envelope {
  final String title; // what tells it apart in its section: "amp" for /env/amp/attack, '' for /env/attack
  final Control attack, decay, sustain, release;
  const Envelope(this.title, this.attack, this.decay, this.sustain, this.release);
  List<Control> get controls => [attack, decay, sustain, release];
}

/// Cutoff across, resonance up.
class XyPad {
  final String title;
  final Control x, y;
  const XyPad(this.title, this.x, this.y);
  List<Control> get controls => [x, y];
}

/// A section's entries in order: a [Control], an [Envelope] or an [XyPad].
class Section {
  final String title;
  final List<Object> items;
  Section(this.title, this.items);
}

const _envStages = ['attack', 'decay', 'sustain', 'release'];
const _padAxes = ['cutoff', 'resonance'];

// (section, group, last segment) for a control: from its path, else from its title's last word.
(String, String, String) _place(Control c) {
  if (c.path.isNotEmpty) {
    final seg = c.path.split('/').where((s) => s.isNotEmpty).toList();
    if (seg.isEmpty) return ('', '', '');
    final group = seg.length > 1 ? seg.sublist(0, seg.length - 1).join('/') : '';
    return (seg.length > 1 ? seg.first : '', group, seg.last.toLowerCase());
  }
  final words = c.title.trim().split(RegExp(r'\s+'));
  return ('', words.sublist(0, words.length - 1).join(' ').toLowerCase(), words.last.toLowerCase());
}

// A widget's title: its group's segments after the section, without a bare "env" ("/amp/env" in section amp: '').
String _title(String section, String group) {
  final parts = group.split(RegExp(r'[/ ]')).where((s) => s.isNotEmpty).toList();
  if (section.isNotEmpty && parts.isNotEmpty && parts.first == section) parts.removeAt(0);
  return parts.where((p) => p != 'env' && p != 'envelope').join(' ');
}

/// [widgets] false: every control on its own, as faders, pickers and toggles.
List<Section> sections(Device d, {bool widgets = true}) {
  final byGroup = <String, Map<String, Control>>{};
  final sectionOf = <String, String>{};
  for (final c in d.controls) {
    if (!widgets || d.kind(c) != Kind.fader || c.type != 'cc') continue;
    final (section, group, last) = _place(c);
    if (_envStages.contains(last) || _padAxes.contains(last)) {
      (byGroup[group] ??= {})[last] = c;
      sectionOf[group] = section;
    }
  }
  final widgetOf = <Control, Object>{};
  for (final e in byGroup.entries) {
    final m = e.value, title = _title(sectionOf[e.key]!, e.key);
    if (_envStages.every(m.containsKey)) {
      final env = Envelope(title, m['attack']!, m['decay']!, m['sustain']!, m['release']!);
      for (final c in env.controls) {
        widgetOf[c] = env;
      }
    }
    if (_padAxes.every(m.containsKey)) {
      final pad = XyPad(title, m['cutoff']!, m['resonance']!);
      for (final c in pad.controls) {
        widgetOf[c] = pad;
      }
    }
  }
  final out = <String, Section>{};
  for (final c in d.controls) {
    final s = out[_place(c).$1] ??= Section(_place(c).$1, []);
    final w = widgetOf[c];
    if (w == null) {
      s.items.add(c);
    } else if (!s.items.contains(w)) {
      s.items.add(w);
    }
  }
  return out.values.toList();
}

// ---- wave shapes ----

enum WaveShape { sine, triangle, saw, square, pulse, noise }

/// The shape a value name stands for ("Saw", "Sawtooth", "Square", "Tri"...), or null.
WaveShape? waveShapeOf(String title) {
  final t = title.toLowerCase().trim();
  if (t.startsWith('sin')) return WaveShape.sine;
  if (t.startsWith('tri')) return WaveShape.triangle;
  if (t.startsWith('saw') || t == 'ramp') return WaveShape.saw;
  if (t.startsWith('sq')) return WaveShape.square;
  if (t.startsWith('pulse') || t == 'pwm') return WaveShape.pulse;
  if (t.startsWith('noise')) return WaveShape.noise;
  return null;
}

// ---- time ----

/// Seconds from a value name such as "20ms", "1s", "1.5 s"; null if it is not a time.
double? secondsOf(String title) {
  final m = RegExp(r'^\s*(\d+(?:\.\d+)?)\s*(ms|s)\s*$').firstMatch(title.toLowerCase());
  if (m == null) return null;
  final v = double.parse(m[1]!);
  return v <= 0 ? null : (m[2] == 'ms' ? v / 1000 : v);
}

/// Hertz from a value name such as "500Hz", "1.2k", "20 kHz"; null if it is not a frequency.
double? hertzOf(String title) {
  final m = RegExp(r'^\s*(\d+(?:\.\d+)?)\s*(hz|k|khz)\s*$').firstMatch(title.toLowerCase());
  if (m == null) return null;
  final v = double.parse(m[1]!);
  return v <= 0 ? null : (m[2] == 'hz' ? v : v * 1000);
}

/// A control's value in seconds, between the named times of its CtrlMapList (on a log scale: the points of a time
/// curve are spaced that way); null without at least two named times.
double? seconds(Device d, Control c) => _between(d, c, secondsOf);

/// The same for a frequency (a cutoff's named points).
double? hertz(Device d, Control c) => _between(d, c, hertzOf);

double? _between(Device d, Control c, double? Function(String) parse) {
  final pts = [
    for (final o in d.maps[c.mapId] ?? const <MapEntry>[])
      if (parse(o.title) case final s?) (o.value, s)
  ]..sort((a, b) => a.$1.compareTo(b.$1));
  if (pts.length < 2) return null;
  var i = 0;
  while (i < pts.length - 2 && c.value > pts[i + 1].$1) {
    i++;
  }
  final (v0, s0) = pts[i];
  final (v1, s1) = pts[i + 1];
  final t = ((c.value - v0) / (v1 - v0)).clamp(0.0, 1.0);
  return math.exp(math.log(s0) + t * (math.log(s1) - math.log(s0)));
}

/// What a control shows: a time or frequency between its named points, else its named point or percent (Device.shown).
String display(Device d, Control c) {
  final s = seconds(d, c);
  if (s != null) return formatSeconds(s);
  final hz = hertz(d, c);
  if (hz != null) return formatHertz(hz);
  return d.shown(c);
}

/// "480 Hz", "1.2 kHz", "12 kHz": two significant figures.
String formatHertz(double hz) {
  if (hz < 1000) return '${_two(hz)} Hz';
  if (hz < 10000) return '${(hz / 1000).toStringAsFixed(1)} kHz';
  return '${(hz / 1000).round()} kHz';
}

/// "4 ms", "27 ms", "350 ms", "1.2 s", "12 s": two significant figures.
String formatSeconds(double s) {
  if (s < 0.01) return '${(s * 1000).toStringAsFixed(1).replaceAll('.0', '')} ms';
  if (s < 1) return '${_two(s * 1000)} ms';
  if (s < 10) return '${s.toStringAsFixed(1)} s';
  return '${s.round()} s';
}

int _two(double v) {
  final p = math.pow(10, (math.log(v) / math.ln10).floor() - 1);
  return ((v / p).round() * p).round();
}

// ---- the editor's shape ----

/// The envelope drawn in a [w] x [h] box: each of attack, decay and release spans up to [seg] across by its knob
/// position, sustain holds for a fixed [plateau] at its level. Points: start, peak, decay end, sustain end, release end.
class EnvShape {
  final double w, h, seg, plateau;
  final double a, dcy, s, r; // fractions 0..1
  EnvShape(this.w, this.h, this.a, this.dcy, this.s, this.r)
      : plateau = w * 0.16,
        seg = w * 0.84 / 3;

  List<(double, double)> get points {
    final x1 = a * seg, x2 = x1 + dcy * seg, x3 = x2 + plateau, ys = (1 - s) * h;
    return [(0, h), (x1, 0), (x2, ys), (x3, ys), (x3 + r * seg, h)];
  }
}

/// The handles: 0 = peak (attack), 1 = decay end (decay across, sustain up/down), 2 = sustain end (sustain), 3 =
/// release end (release). Their positions are points 1..4.
enum Handle { attack, decay, sustain, release }

/// The handle nearest to (x, y).
Handle nearestHandle(EnvShape e, double x, double y) {
  final p = e.points.sublist(1);
  var best = 0;
  double dist(int i) => (math.pow(p[i].$1 - x, 2) + math.pow(p[i].$2 - y, 2)).toDouble();
  for (var i = 1; i < 4; i++) {
    if (dist(i) < dist(best)) best = i;
  }
  return Handle.values[best];
}

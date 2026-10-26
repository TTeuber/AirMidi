// The synth page's layout from the control paths (M2-117 paramPath; conventions in docs/system/README.md, "Control
// paths"): one section per first path segment, in the order of the best control in it, and widgets for the groups
// an editor recognises. So far one: attack, decay, sustain and release under one parent path become an envelope.
// A device that sends no paths gets the same from its titles ("Amp Attack", "Amp Decay", ...).
import 'dart:math' as math;

import 'controls.dart';

class Envelope {
  final String title; // the group's own name: "amp" for /env/amp/attack, '' for /env/attack
  final Control attack, decay, sustain, release;
  const Envelope(this.title, this.attack, this.decay, this.sustain, this.release);
  List<Control> get controls => [attack, decay, sustain, release];
}

/// A section's entries in order: a [Control] or an [Envelope].
class Section {
  final String title;
  final List<Object> items;
  Section(this.title, this.items);
}

const _stages = ['attack', 'decay', 'sustain', 'release'];

// (section, group, stage) for a control: from its path, else from its title's last word.
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

List<Section> sections(Device d) {
  final byGroup = <String, Map<String, Control>>{};
  for (final c in d.controls) {
    if (d.kind(c) != Kind.fader || c.type != 'cc') continue;
    final (_, group, stage) = _place(c);
    if (_stages.contains(stage)) (byGroup[group] ??= {})[stage] = c;
  }
  final envOf = <Control, Envelope>{};
  for (final e in byGroup.entries) {
    final m = e.value;
    if (m.length < 4) continue;
    final parts = e.key.split('/');
    final env = Envelope(parts.length > 1 ? parts.sublist(1).join(' ') : '', m['attack']!, m['decay']!, m['sustain']!,
        m['release']!);
    for (final c in env.controls) {
      envOf[c] = env;
    }
  }
  final out = <String, Section>{};
  for (final c in d.controls) {
    final s = out[_place(c).$1] ??= Section(_place(c).$1, []);
    final env = envOf[c];
    if (env == null) {
      s.items.add(c);
    } else if (!s.items.contains(env)) {
      s.items.add(env);
    }
  }
  return out.values.toList();
}

// ---- time ----

/// Seconds from a value name such as "20ms", "1s", "1.5 s"; null if it is not a time.
double? secondsOf(String title) {
  final m = RegExp(r'^\s*(\d+(?:\.\d+)?)\s*(ms|s)\s*$').firstMatch(title.toLowerCase());
  if (m == null) return null;
  final v = double.parse(m[1]!);
  return v <= 0 ? null : (m[2] == 'ms' ? v / 1000 : v);
}

/// A control's value in seconds, between the named times of its CtrlMapList (on a log scale: the points of a time
/// curve are spaced that way); null without at least two named times.
double? seconds(Device d, Control c) {
  final pts = [
    for (final o in d.maps[c.mapId] ?? const <MapEntry>[])
      if (secondsOf(o.title) case final s?) (o.value, s)
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

// Controller lanes' names and scales (docs/daw.md "The piano roll"): a lane is named by what it sends ('cc74',
// 'bend', 'pressure', 'program'; an MPE note's own 'note:bend', 'note:pressure', 'note:timbre'). With the track's
// MIDI-CI control list the lane takes the synth's own name for it ("Cutoff" for CC 74) and its value display (Hz,
// ms, named points); without one, the General MIDI name.
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/shared/synth/controls.dart';
import 'package:airmidi/shared/synth/groups.dart' show display;

const _gm = {
  1: 'mod wheel',
  2: 'breath',
  7: 'volume',
  10: 'pan',
  11: 'expression',
  64: 'sustain',
  71: 'resonance',
  74: 'brightness',
};

/// The lanes a "+ lane" menu offers: the synth's own controls first, then the common ones it does not name.
List<(String, String)> laneChoices(Device? d) {
  final own = <(String, String)>[
    if (d != null)
      for (final c in d.controls)
        if (laneKeyOf(c) case final k?) (c.title, k),
  ];
  final named = {for (final (_, k) in own) k};
  return [
    ...own,
    for (final (title, k) in [
      for (final e in _gm.entries) ('${e.value} (CC ${e.key})', 'cc${e.key}'),
      ('pitch bend', 'bend'),
      ('channel pressure', 'pressure'),
      ('program', 'program'),
    ])
      if (!named.contains(k)) (title, k),
  ];
}

/// The lane a MIDI-CI control moves, if a lane can move it.
String? laneKeyOf(Control c) => switch (c.type) {
      'cc' when c.index.isNotEmpty => 'cc${c.index.first}',
      'chPress' => 'pressure',
      'pBend' => 'bend',
      _ => null,
    };

Control? _control(String key, Device? d) {
  if (d == null) return null;
  for (final c in d.controls) {
    if (laneKeyOf(c) == key) return c;
  }
  return null;
}

String laneTitle(String key, Device? d) {
  final c = _control(key, d);
  if (c != null) return c.title;
  if (key.startsWith('note:')) return 'note ${key.substring(5)}';
  if (key.startsWith('cc')) {
    final n = int.tryParse(key.substring(2)) ?? 0;
    return _gm[n] ?? 'CC $n';
  }
  return switch (key) { 'bend' => 'pitch bend', 'pressure' => 'pressure', 'program' => 'program', _ => key };
}

bool laneCentred(String key) => key == 'bend' || key == 'note:bend';

/// A lane's value from 0..1 (its height), at the resolution it is sent with: bends 14 bit, the rest 7.
int laneValueAt(String key, double f) {
  f = f.clamp(0.0, 1.0);
  return laneCentred(key) ? up14to32((f * 16383).round()) : up7to32((f * 127).round());
}

double laneFraction(String key, int v) => laneCentred(key) ? down32to14(v) / 16383 : down32to7(v) / 127;

/// What a point shows: the synth's own reading if it has one, else the MIDI value (bends as +-percent).
String laneDisplay(String key, int v, Device? d) {
  final c = _control(key, d);
  if (c != null) {
    final was = c.value;
    c.value = v;
    final s = display(d!, c);
    c.value = was;
    return s;
  }
  if (laneCentred(key)) return '${((down32to14(v) - 8192) / 8192 * 100).round()}%';
  return '${down32to7(v)}';
}

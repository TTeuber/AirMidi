// Sections, the envelope group and its times, against daisy_synth's real MIDI-CI resources
// (fixtures/daisy_synth_api.json, from devices/daisy_synth/host/build/ci).
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:airmidi/shared/synth/controls.dart';
import 'package:airmidi/shared/synth/groups.dart';
import 'package:flutter_test/flutter_test.dart';

Device daisy() =>
    Device.fromApi(jsonDecode(File('test/fixtures/daisy_synth_api.json').readAsStringSync()) as Map<String, dynamic>);

String names(Section s) =>
    [for (final i in s.items) i is Envelope ? 'ENV' : (i is XyPad ? 'XY' : (i as Control).title)].join(',');

Map<String, dynamic> ctl(String title, int cc, {String? path, String? map}) => {
      'title': title,
      'channel': 1,
      'ctrlType': 'cc',
      'ctrlIndex': [cc],
      'priority': 1,
      'default': 0,
      if (path != null) 'paramPath': path,
      if (map != null) 'ctrlMapId': map,
    };

void main() {
  test('daisy_synth: sections by path; cutoff + resonance a pad, each ADSR an envelope', () {
    final s = sections(daisy());
    expect(s.map((x) => x.title), ['filter', 'amp', 'fx', 'osc']);
    expect(names(s[0]), 'XY,Env Amt,ENV');
    final pad = s[0].items[0] as XyPad, fenv = s[0].items[2] as Envelope;
    expect([pad.x.title, pad.y.title], ['Cutoff', 'Resonance']);
    expect([for (final c in fenv.controls) c.title],
        ['Filter Attack', 'Filter Decay', 'Filter Sustain', 'Filter Release']);
    expect(fenv.title, ''); // /filter/env in section filter: nothing more to say
    final amp = s[1].items.single as Envelope;
    expect([for (final c in amp.controls) c.title], ['Amp Attack', 'Amp Decay', 'Amp Sustain', 'Amp Release']);
    expect(names(s[3]), 'Wave,Glide,Detune,Vibrato,Glide Time,Bend');
  });

  test('graphics off: the same sections, every control on its own', () {
    final s = sections(daisy(), widgets: false);
    expect(s.map((x) => x.title), ['filter', 'amp', 'fx', 'osc']);
    expect(names(s[0]), 'Cutoff,Resonance,Env Amt,Filter Attack,Filter Decay,Filter Sustain,Filter Release');
  });

  test('ties in priority keep the device\'s order', () {
    final d = daisy();
    expect(d.controls.take(5).map((c) => c.title), ['Cutoff', 'Resonance', 'Env Amt', 'Amp Attack', 'Amp Decay']);
  });

  test('two envelopes under one section, named by their group', () {
    final d = Device.fromApi({
      'controls': [
        for (final (g, n) in [('amp', 0), ('filter', 10)])
          for (final (i, st) in ['attack', 'decay', 'sustain', 'release'].indexed)
            ctl(st, 20 + n + i, path: '/env/$g/$st'),
      ],
    });
    final s = sections(d);
    expect(s.single.title, 'env');
    expect([for (final e in s.single.items) (e as Envelope).title], ['amp', 'filter']);
  });

  test('no paths: an envelope from the titles, the rest a flat list', () {
    final d = Device.fromApi({
      'controls': [
        ctl('Cutoff', 74),
        ctl('Amp Attack', 73),
        ctl('Amp Decay', 75),
        ctl('Amp Sustain', 79),
        ctl('Amp Release', 72),
        ctl('Filter Attack', 80), // only one stage: stays a fader
      ],
    });
    final s = sections(d);
    expect(s.single.title, '');
    expect(names(s.single), 'Cutoff,ENV,Filter Attack');
  });

  test('three of four stages make no envelope', () {
    final d = Device.fromApi({
      'controls': [
        ctl('a', 1, path: '/env/attack'),
        ctl('d', 2, path: '/env/decay'),
        ctl('r', 3, path: '/env/release')
      ],
    });
    expect(names(sections(d).single), 'a,d,r');
  });

  test('times between the named points, on a log scale', () {
    final d = daisy();
    final attack = d.controls.firstWhere((c) => c.title == 'Amp Attack');
    attack.value = 0;
    expect(seconds(d, attack), closeTo(0.001, 1e-9));
    attack.value = kMax32;
    expect(seconds(d, attack), closeTo(3, 1e-9));
    attack.value = 1607128832; // the 20ms point
    expect(seconds(d, attack), closeTo(0.020, 1e-9));
    attack.value = (1607128832 + 2098696448) ~/ 2; // halfway from 20 to 50 ms: the geometric mean
    expect(seconds(d, attack), closeTo(0.0316, 1e-3));
    final sustain = d.controls.firstWhere((c) => c.title == 'Amp Sustain');
    expect(seconds(d, sustain), isNull); // no map
  });

  test('the synth\'s own curve agrees with the interpolation', () {
    // daisy_synth: attack = 1 ms * 2^(11.55 x) (engine/controls.cpp attackSec)
    final d = daisy();
    final attack = d.controls.firstWhere((c) => c.title == 'Amp Attack');
    for (final x in [0.1, 0.37, 0.5, 0.83]) {
      attack.value = (x * kMax32).round();
      final want = 0.001 * math.pow(2, 11.55 * x);
      expect(seconds(d, attack)! / want, closeTo(1, 0.01), reason: 'x=$x');
    }
  });

  test('frequencies between the named points: the cutoff', () {
    final d = daisy();
    final cutoff = d.controls.firstWhere((c) => c.title == 'Cutoff');
    // (not up to the top: the synth's top is 20.48 kHz, its last point is named "20k")
    for (final x in [0.0, 0.3, 0.5, 0.77]) {
      cutoff.value = (x * kMax32).round();
      final want = 20 * math.pow(2, 10 * x); // engine/controls.cpp cutoffHz
      expect(hertz(d, cutoff)! / want, closeTo(1, 0.01), reason: 'x=$x');
    }
    expect([
      for (final t in ['500Hz', '1.2k', '20 kHz', '3s']) hertzOf(t)
    ], [
      500,
      1200,
      20000,
      null
    ]);
    expect([
      for (final hz in [480.0, 1234.0, 12400.0]) formatHertz(hz)
    ], [
      '480 Hz',
      '1.2 kHz',
      '12 kHz'
    ]);
  });

  test('wave shapes from value names', () {
    expect([
      for (final t in ['Saw', 'Sawtooth', 'Square', 'Tri', 'Sine', 'Pulse', 'Noise', 'Super']) waveShapeOf(t)
    ], [
      WaveShape.saw,
      WaveShape.saw,
      WaveShape.square,
      WaveShape.triangle,
      WaveShape.sine,
      WaveShape.pulse,
      WaveShape.noise,
      null,
    ]);
  });

  test('time names', () {
    expect(secondsOf('20ms'), 0.02);
    expect(secondsOf('1.5 s'), 1.5);
    expect(secondsOf('1k'), isNull);
    expect(secondsOf('0ms'), isNull);
    expect([
      for (final s in [0.0012, 0.0045, 0.0273, 0.35, 1.23, 12.4]) formatSeconds(s)
    ], [
      '1.2 ms',
      '4.5 ms',
      '27 ms',
      '350 ms',
      '1.2 s',
      '12 s'
    ]);
  });

  test('shape: segments by knob position, sustain at its level', () {
    final e = EnvShape(300, 100, 0, 1, 0.5, 0.5);
    final p = e.points;
    expect(e.seg, closeTo(84, 1e-9));
    expect(p[1], (0.0, 0.0)); // no attack: the peak at the start
    expect(p[2].$1, closeTo(84, 1e-9));
    expect(p[2].$2, 50);
    expect(p[3].$1, closeTo(84 + 48, 1e-9));
    expect(p[4].$1, closeTo(84 + 48 + 42, 1e-9));
  });

  test('handles: the nearest one', () {
    final e = EnvShape(300, 100, 0.5, 0.5, 0.5, 0.5);
    final p = e.points;
    expect(nearestHandle(e, p[1].$1 + 3, 5), Handle.attack);
    expect(nearestHandle(e, p[2].$1, p[2].$2 + 10), Handle.decay);
    expect(nearestHandle(e, p[3].$1 + 5, 0), Handle.sustain);
    expect(nearestHandle(e, 300, 100), Handle.release);
  });
}

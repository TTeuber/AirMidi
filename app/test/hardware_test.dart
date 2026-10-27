// DAW A6, hardware: program and settings sent first, parameter locks, the click and count-in, latency offsets.
import 'dart:async';
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/player.dart';
import 'package:airmidi/desktop/daw/project_file.dart';
import 'package:airmidi/desktop/daw/smf.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:midi_engine/midi_engine.dart';

const ms = 1000000;
final tl = Timeline(0, 120); // 500 ms a beat

List<String> show(List<SeqEvent> ev) {
  sortEvents(ev);
  return [for (final e in ev) '${(e.tNs / ms).round()} ${e.bytes}'];
}

int cc(int v) => up7to32(v);

/// The engine's clock and ports, without CoreMIDI: what is sent is kept with the time it was asked for.
class FakeEngine implements MidiEngine {
  int now = 0;
  final sent = <(int, List<int>)>[];
  final _events = StreamController<MidiInEvent>.broadcast(), _ports = StreamController<void>.broadcast();

  @override
  int get nowNs => now;
  @override
  List<MidiPortInfo> get outputs => const [MidiPortInfo(0, true, 'out', 0)];
  @override
  List<MidiPortInfo> get inputs => const [];
  @override
  Stream<MidiInEvent> get events => _events.stream;
  @override
  Stream<void> get portsChanged => _ports.stream;
  @override
  int open(MidiPortInfo p) => 1;
  @override
  void close(int handle) {}
  @override
  int clear(int handle) => 0;
  @override
  void send(int handle, List<int> bytes, {int atNs = 0}) => sent.add((atNs, bytes));
  @override
  dynamic noSuchMethod(Invocation i) => super.noSuchMethod(i);

  List<String> get shown => [for (final (t, b) in sent) '${(t / ms).round()} $b'];
}

void main() {
  test('setup: bank, program, then the saved settings, on the track\'s channel (an MPE track\'s manager)', () {
    final p = Project(tracks: [
      Track('a', port: 'x', channel: 3, program: 4, bankMsb: 1, bankLsb: 2, state: {'cc74': cc(100), 'cc1': cc(5)}),
      Track('b', port: 'y', mpe: true, channel: 5, program: 9),
      Track('c', program: 3), // sends nowhere
      Track('d', port: 'z', state: {'cc7': cc(90)})..mute = true, // muted: still set
    ]);
    expect(show(setupEvents(p, 0)), [
      '0 [179, 0, 1]', '0 [179, 32, 2]', '0 [195, 4]', '0 [179, 1, 5]', '0 [179, 74, 100]', //
      '0 [192, 9]', '0 [176, 7, 90]',
    ]);
  });

  group('parameter locks', () {
    // hats on every 8th, the third one's cutoff locked; the track's own cutoff 40
    (Project, Pattern) beat({int? own = 40}) {
      final pat = Pattern(gmDrumRows(), steps: 8);
      final hat = pat.rows.indexWhere((r) => r.name == 'hat'), kick = pat.rows.indexWhere((r) => r.name == 'kick');
      for (var s = 0; s < 8; s += 2) {
        pat.toggle(hat, s);
      }
      pat.toggle(kick, 4);
      pat.cells[hat][4]!.locks['cc74'] = cc(110);
      final t =
          Track('d', port: 'x', channel: 9, clips: [Clip('b', 0, 2, pat)], state: {if (own != null) 'cc74': cc(own)});
      return (Project(tracks: [t]), pat);
    }

    test('a lock sends before its step, the other steps send the own value; a locked step wins over one beside it', () {
      final (p, _) = beat();
      final cutoffs = show(compile(p, 0, 2, tl.nsAt)).where((e) => e.contains('[185, 74,')).toList();
      // 0, 250, 500 (kick unlocked + hat locked at the same time: the lock only), 750 ms
      expect(cutoffs, ['0 [185, 74, 40]', '250 [185, 74, 40]', '500 [185, 74, 110]', '750 [185, 74, 40]']);
      final at500 = show(compile(p, 0, 2, tl.nsAt)).where((e) => e.startsWith('500 ')).toList();
      expect(at500.first, '500 [185, 74, 110]'); // before the notes
    });

    test('no own value: the lock lasts until the next', () {
      final (p, _) = beat(own: null);
      final cutoffs = show(compile(p, 0, 2, tl.nsAt)).where((e) => e.contains('[185, 74,')).toList();
      expect(cutoffs, ['500 [185, 74, 110]']);
    });

    test('the own value falls back on the synth list\'s default', () {
      final (p, _) = beat(own: null);
      p.tracks.single.device = {
        'controls': [
          {
            'title': 'Cutoff',
            'ctrlType': 'cc',
            'ctrlIndex': [74],
            'default': cc(64)
          }
        ]
      };
      final cutoffs = show(compile(p, 0, 2, tl.nsAt)).where((e) => e.contains('[185, 74,')).toList();
      expect(cutoffs.first, '0 [185, 74, 64]');
    });

    test('windows join: the same events however the song is cut', () {
      final (p, _) = beat();
      final whole = show(compile(p, 0, 8, tl.nsAt));
      final r = math.Random(3);
      for (var n = 0; n < 10; n++) {
        final cuts = [0.0, for (var i = 0; i < 5; i++) r.nextDouble() * 8, 8.0]..sort();
        final parts = <SeqEvent>[];
        for (var i = 0; i + 1 < cuts.length; i++) {
          parts.addAll(compile(p, cuts[i], cuts[i + 1], tl.nsAt));
        }
        expect(show(parts), whole);
      }
    });

    test('to MIDI clip: locks become lane points and play the same', () {
      final (p, pat) = beat();
      final c = p.tracks.single.clips.single, t = p.tracks.single;
      final played = show(compile(p, 0, 2, tl.nsAt));
      c.content = patternToMidi(pat, c.id, base: (k) => t.state[k]);
      expect((c.content as MidiContent).lanes['cc74']!.length, 4);
      expect(show(compile(p, 0, 2, tl.nsAt)), played);
    });

    test('the chase sends the last lock or own value before the start', () {
      final (p, _) = beat();
      expect(show(chase(p, 1.1, 0)), ['0 [185, 74, 110]']);
      expect(show(chase(p, 1.6, 0)), ['0 [185, 74, 40]']);
    });
  });

  test('files: program, bank, settings, the box, locks, the click and count-in read back', () {
    final pat = Pattern([PatternRow(60, 'c')])..toggle(0, 0);
    pat.cells[0][0]!.locks.addAll({'cc74': cc(3), 'bend': up14to32(9000)});
    final p = Project(tracks: [
      Track('a', port: 'x', program: 7, bankMsb: 2, state: {'cc1': cc(9)}, box: 'abc123', clips: [Clip('p', 0, 4, pat)])
    ], click: Click(port: 'drums', channel: 3, note: 75, on: true), countIn: 2);
    final back = decodeProject(encodeProject(p));
    expect(encodeProject(back), encodeProject(p));
    final t = back.tracks.single;
    expect((t.program, t.bankMsb, t.bankLsb, t.box, t.state['cc1']), (7, 2, null, 'abc123', cc(9)));
    expect(
        (back.click.port, back.click.channel, back.click.note, back.click.on, back.countIn), ('drums', 3, 75, true, 2));
    expect((t.clips.single.content as Pattern).cells[0][0]!.locks['bend'], up14to32(9000));
    // an older file: the defaults
    final old = decodeProject('{"format": "airmidi project", "version": 1, "tracks": [{"name": "t"}]}');
    expect((old.click.port, old.click.note, old.countIn, old.tracks.single.program), (null, 37, 1, null));
  });

  test('MIDI file export: the program and settings at the start of the track', () {
    final p = Project(tracks: [
      Track('a', port: 'x', program: 4, state: {
        'cc74': cc(20)
      }, clips: [
        Clip('c', 0, 4, MidiContent(4, notes: [Note(0, 1, 60)]))
      ])
    ]);
    final f = exportSmf(p);
    final body = String.fromCharCodes(f);
    final i = body.lastIndexOf('MTrk') + 8;
    // the track name, then program 4 and CC 74 = 20 at delta 0, then the note
    final events = f.sublist(i);
    final name = 4 + events[3];
    expect(events.sublist(name, name + 7), [0, 0xC0, 4, 0, 0xB0, 74, 20]);
  });

  group('the player', () {
    late FakeEngine engine;
    late Project p;
    late DawPlayer player;

    setUp(() {
      engine = FakeEngine();
      p = Project(tracks: [
        Track('s', port: 'out', program: 2, clips: [
          Clip('c', 0, 4, MidiContent(4, notes: [Note(0, 0.5, 60)]))
        ])
          ..armed = true
          ..input = kKeysInput,
      ], click: Click(port: 'out'));
      player = DawPlayer(p, History(p), engine);
    });

    tearDown(() => player.dispose());

    Future<void> runTo(int msNow) async {
      while (engine.now < msNow * ms) {
        engine.now += 20 * ms;
        await Future.delayed(const Duration(milliseconds: 21));
      }
    }

    test('recording from stopped counts in a bar with the click, then the song starts', () async {
      player.play(record: true);
      expect(player.countInLeft, closeTo(4, 0.2));
      await runTo(2300);
      final s = engine.shown;
      expect(s.first, '0 [192, 2]'); // the program, at once
      // the count-in: 60 ms to start, four beats of 500 ms, the first one louder; the song's note on the fifth
      expect(s.where((e) => e.contains('[153, 37,')).toList(),
          ['60 [153, 37, 127]', '560 [153, 37, 80]', '1060 [153, 37, 80]', '1560 [153, 37, 80]']);
      expect(s.where((e) => e.contains('[144, 60,')).toList(), ['2060 [144, 60, 100]']);
      expect(player.countInLeft, 0);
      expect(player.takes.length, 1);
    });

    test('the click on: every beat of the song, the bar\'s first louder', () async {
      p.click.on = true;
      player.play();
      await runTo(1400);
      expect(engine.shown.where((e) => e.contains('[153, 37,')).toList(),
          ['60 [153, 37, 127]', '560 [153, 37, 80]', '1060 [153, 37, 80]']);
    });

    test('an output\'s latency offset sends its events that much early', () async {
      player.outLatency['out'] = 25;
      player.play();
      await runTo(600);
      // the first beat moves to 85 ms (the start plus the lead), its events go out 25 ms before it
      expect(engine.shown.where((e) => e.contains('[144, 60,')).toList(), ['60 [144, 60, 100]']);
    });
  });
}

import 'dart:io';

import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/project_file.dart';
import 'package:airmidi/desktop/daw/smf.dart';
import 'package:airmidi/desktop/daw/store.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// A song with one of everything.
Project song() {
  final pat = Pattern(gmDrumRows(), swing: 0.58, steps: 12);
  for (final s in [0, 4, 8]) {
    pat.toggle(5, s);
  }
  pat.toggle(2, 3);
  pat.cells[2][3]!
    ..probability = 0.5
    ..nudge = -0.1
    ..repeat = 2
    ..gate = 0.8
    ..velocity = up7to16(77);
  pat.rows[0].mute = true;
  final midi = MidiContent(4, notes: [
    Note(0, 1, 60, velocity: up7to16(90), release: up7to16(30)),
    Note(1.5, 0.5, 64),
  ], lanes: {
    'cc1': [const Point(0, 0), const Point(2, 0x80000000)],
    'bend': [const Point(3, 0xA0000000)],
    'cc74': [const Point(0, 0, ramp: true), Point(2, up7to32(127))],
  }, other: [
    const RawEvent(0.5, [0xA0, 60, 40])
  ]);
  final mpe = MidiContent(2, notes: [
    Note(0, 1, 62, expression: {
      'bend': [const Point(0, 0x80000000), const Point(0.5, 0x90000000)],
      'timbre': [const Point(0.2, 0x40000000)],
    }),
    Note(0.5, 1, 69),
  ]);
  return Project(bpm: 97, loopOn: true, loopStart: 4, loopEnd: 12, startBeat: 2, clockPorts: {
    'TR-8'
  }, tracks: [
    Track('drums', port: 'TR-8', channel: 9, clips: [Clip('beat', 0, 6, pat), Clip('beat 2', 8, 4, pat.copy())]),
    Track('bass', port: 'box', channel: 1, clips: [
      Clip('line', 4, 8, midi, offset: 1)
    ], device: {
      'info': {'manufacturer': 'Daisy', 'model': 'Poly Synth'},
      'controls': [
        {
          'title': 'Cutoff',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [74]
        }
      ],
    })
      ..mute = true,
    Track('strips', port: 'synth', mpe: true, input: 'keys page', clips: [Clip('take', 0, 4, mpe, loops: false)])
      ..monitor = false,
  ]);
}

List<String> played(Project p, {bool all = false, bool ticks = false}) {
  final tl = Timeline(0, p.bpm);
  int at(double b) => ticks ? (b * kPpq).round() : tl.nsAt(b);
  if (all) {
    for (final t in p.tracks) {
      t.mute = false;
    }
  }
  final ev = compile(p, 0, songLength(p), at);
  sortEvents(ev);
  return [for (final e in ev) '$e'];
}

void main() {
  test('a project file reads back as the same project: the same file again and the same MIDI', () {
    final p = song();
    final text = encodeProject(p);
    final back = decodeProject(text);
    expect(encodeProject(back), text);
    expect(played(back), played(p));
    expect(back.tracks[2].clips.single.id, p.tracks[2].clips.single.id); // ids kept: the same probabilities
    expect((back.tracks[1].mute, back.tracks[2].mpe, back.tracks[2].monitor), (true, true, false));
    expect(back.clockPorts, {'TR-8'});
    expect(back.tracks[1].device!['controls'], isNotEmpty); // the synth's list goes with the project
    expect((back.tracks[1].clips.single.content as MidiContent).lanes['cc74']!.first.ramp, isTrue);
    expect(played(back, all: true), played(p, all: true)); // the ramp plays the same after reopening
    expect(newId() > p.tracks[2].clips.single.id, isTrue);
    expect(text, contains('"format": "airmidi project"'));
  });

  test('files that are not projects, or newer ones, are refused', () {
    expect(() => decodeProject('{"format": "something else"}'), throwsFormatException);
    expect(() => decodeProject('{"format": "airmidi project", "version": 99}'), throwsFormatException);
  });

  test('MIDI file export: header, tempo, a track per audible track, an MPE setup first', () {
    final p = song();
    final f = exportSmf(p);
    expect(String.fromCharCodes(f.sublist(0, 4)), 'MThd');
    expect(f.sublist(8, 14), [0, 1, 0, 3, 480 >> 8, 480 & 0xFF]); // type 1; tempo track + drums + strips (bass muted)
    final tempo = (60e6 / 97).round();
    final at = String.fromCharCodes(f).indexOf(String.fromCharCodes([0xFF, 0x51, 3]));
    expect([f[at + 3], f[at + 4], f[at + 5]], [(tempo >> 16) & 0xFF, (tempo >> 8) & 0xFF, tempo & 0xFF]);
    expect(String.fromCharCodes(f).contains(String.fromCharCodes([0xB0, 101, 0, 0, 0xB0, 100, 6, 0, 0xB0, 6, 15])),
        isTrue);
  });

  test('export then import: the same notes at the same places', () {
    final p = song();
    final back = importSmf(exportSmf(p));
    expect(back.bpm, closeTo(97, 0.01));
    expect(back.tracks.map((t) => (t.name, t.channel, t.mpe)).toList(), [('drums', 9, false), ('strips', 0, true)]);
    // the drum track plays the same notes on the same ticks (pattern rendered, first-pass probabilities)
    final drumsOnly = Project(bpm: 97, tracks: [p.tracks[0]]);
    final drumsBack = Project(bpm: 97, tracks: [back.tracks[0]..port = 'TR-8']);
    expect(played(drumsBack, ticks: true), played(drumsOnly, ticks: true)); // a MIDI file is in whole ticks
    // MPE: two notes with the first one's bend and timbre
    final m = back.tracks[1].clips.single.content as MidiContent;
    expect(m.notes.map((n) => n.pitch).toList(), [62, 69]);
    expect(m.lanes, isEmpty); // the MPE setup is not kept as controllers
    expect(m.notes.first.expression['bend']!.length, 2);
    expect(m.notes.first.expression['timbre']!.single.t, closeTo(0.2, 1 / 480));
  });

  test('import: a type 0 file with running status and two channels becomes two tracks', () {
    final f = <int>[
      ...'MThd'.codeUnits, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, //
      ...'MTrk'.codeUnits, 0, 0, 0, 25,
      0, 0x90, 60, 100, // ch 1 note on
      48, 62, 90, // running status: another note on, half a beat later
      48, 0x89, 36, 0, // ch 10: a note off with no note on (ignored)
      0, 0x99, 38, 120, // ch 10 snare
      96, 0x80, 60, 0,
      0, 0xFF, 0x2F, 0,
    ];
    final got = importSmf(f);
    expect(got.tracks.map((t) => (t.name, t.channel)).toList(), [('track 1 ch 1', 0), ('track 1 ch 10', 9)]);
    final a = got.tracks[0].clips.single.content as MidiContent;
    expect([for (final n in a.notes) (n.start, n.pitch)], [(0.0, 60), (0.5, 62)]);
    expect(a.notes.first.length, 2.0);
    expect(got.tracks[0].clips.single.length, 4);
    expect(() => importSmf([1, 2, 3]), throwsFormatException);
  });

  test('the store: autosaves an untitled project, saves under a name, reopens the last one on start', () async {
    final dir = await Directory.systemTemp.createTemp('airmidi_store');
    ProjectStore.homeForTests = dir.path;
    SharedPreferences.setMockInitialValues({});
    final p = Project(tracks: [Track('a')]);
    final s = ProjectStore(p, History(p));
    await s.start();
    p.tracks.first.name = 'changed';
    await s.save();
    expect(File(ProjectStore.autosavePath).readAsStringSync(), contains('changed'));
    await s.saveAs('${ProjectStore.defaultDir}/my song');
    expect(s.name, 'my song');
    expect(File('${ProjectStore.defaultDir}/my song.airmidi').existsSync(), isTrue);
    p.tracks.first.name = 'later';
    await s.save();
    s.dispose();
    // the app again: the last project opens
    final q = Project();
    final s2 = ProjectStore(q, History(q));
    await s2.start();
    expect((s2.name, q.tracks.single.name), ('my song', 'later'));
    expect(s2.recent.first, endsWith('my song.airmidi'));
    s2.dispose();
    ProjectStore.homeForTests = null;
    await dir.delete(recursive: true);
  });
}

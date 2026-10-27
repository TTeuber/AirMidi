import 'dart:math' as math;

import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/record.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:flutter_test/flutter_test.dart';

const ms = 1000000;
final tl = Timeline(0, 120); // 500 ms a beat
int at(double b) => tl.nsAt(b);

List<String> show(List<SeqEvent> ev) {
  sortEvents(ev);
  return [for (final e in ev) '${(e.tNs / ms).round()} ${e.bytes}'];
}

Project oneClip(Clip c, {bool mpe = false, int channel = 0}) =>
    Project(tracks: [Track('t', port: 'p', channel: channel, mpe: mpe, clips: [c])]);

void main() {
  test('MIDI 1.0 values up to MIDI 2.0 and back', () {
    expect([up7to16(0), up7to16(64), up7to16(127)], [0, 0x8000, 0xFFFF]);
    expect([up7to32(127), up14to32(8192), up14to32(16383)], [0xFFFFFFFF, 0x80000000, 0xFFFFFFFF]);
    for (var v = 0; v < 128; v++) {
      expect(down16to7(up7to16(v)), v);
      expect(down32to7(up7to32(v)), v);
    }
    for (final v in [0, 1, 8191, 8192, 8193, 16383]) {
      expect(down32to14(up14to32(v)), v);
    }
  });

  test('a MIDI clip: notes from its start, cut at its end, repeated when it is longer, from its offset', () {
    final m = MidiContent(2, notes: [Note(0, 0.5, 60), Note(1.5, 1, 62)]); // the second runs past the content end
    final c = Clip('c', 4, 3, m); // two beats of content in three beats: one and a half passes
    expect(show(compile(oneClip(c), 0, 16, at)), [
      '2000 [144, 60, 100]', '2250 [128, 60, 0]', //
      '2750 [144, 62, 100]', '3000 [128, 62, 0]', // cut where the content repeats
      '3000 [144, 60, 100]', '3250 [128, 60, 0]', //
    ]); // the second pass's 62 would start at 3.5 + 4 = 7.5 > the clip's end (7)
    c
      ..offset = 1.5
      ..length = 1;
    // from 1.5 into the content: 62 until the content ends, then it wraps and 60 starts half a beat in
    expect(show(compile(oneClip(c), 0, 16, at)),
        ['2000 [144, 62, 100]', '2250 [128, 62, 0]', '2250 [144, 60, 100]', '2500 [128, 60, 0]']);
    c.loops = false;
    c.length = 6;
    expect(compile(oneClip(c), 0, 16, at).where((e) => e.bytes[0] == 0x90).length, 1); // 62 at offset 1.5 only
  });

  test('windows join without gaps or doubles (MIDI clip, pattern with swing, nudge, repeats, probability)', () {
    final pat = Pattern(gmDrumRows(), swing: 0.62);
    for (var s = 0; s < 16; s++) {
      pat.toggle(2, s);
    }
    pat.cells[2][3]!.nudge = -0.3;
    pat.cells[2][15]!.nudge = 0.4;
    pat.cells[2][6]!.repeat = 3;
    pat.cells[2][9]!.probability = 0.5;
    final mid = MidiContent(3, notes: [Note(0.1, 0.4, 60), Note(2.9, 0.5, 64)], lanes: {
      'cc1': [const Point(0.3, 0x40000000), const Point(2.5, 0)]
    });
    final p = Project(tracks: [
      Track('drums', port: 'd', channel: 9, clips: [Clip('beat', 0, 8, pat)]),
      Track('synth', port: 's', clips: [Clip('line', 1, 7, mid)]),
    ]);
    final all = show(compile(p, 0, 8, at));
    final rnd = math.Random(3);
    for (var trial = 0; trial < 20; trial++) {
      final pieces = <SeqEvent>[];
      var from = 0.0;
      while (from < 8) {
        final to = math.min(8.0, from + rnd.nextDouble() * 0.7);
        pieces.addAll(compile(p, from, to, at));
        from = to;
      }
      expect(show(pieces), all);
    }
  });

  test('pattern: four on the floor, swing, repeats, a muted row, probability the same every time', () {
    final pat = Pattern(gmDrumRows());
    final kick = pat.rows.indexWhere((r) => r.name == 'kick'), hat = pat.rows.indexWhere((r) => r.name == 'hat');
    for (final s in [0, 4, 8, 12]) {
      pat.toggle(kick, s);
    }
    final p = oneClip(Clip('beat', 0, 4, pat), channel: 9);
    expect(show(compile(p, 0, 2, at)), [
      '0 [153, 36, 100]', '63 [137, 36, 0]', // half a sixteenth at 120 bpm: 62.5 ms
      '500 [153, 36, 100]', '563 [137, 36, 0]',
    ]);
    pat.toggle(hat, 1);
    pat.swing = 0.75; // the second sixteenth of a pair lands on the third of its eighth
    final hatOn = compile(p, 0, 1, at).firstWhere((e) => e.bytes[1] == 42 && e.bytes[0] == 0x99);
    expect(hatOn.tNs, at(0.25 * 1.5));
    pat.cells[hat][1]!.repeat = 2;
    expect(compile(p, 0, 1, at).where((e) => e.bytes[1] == 42 && e.bytes[0] == 0x99).length, 2);
    pat.rows[kick].mute = true;
    expect(compile(p, 0, 4, at).where((e) => e.bytes[1] == 36), isEmpty);
    // probability: about half of 400 hits, and the same choice for the same pass every time
    final prob = Pattern([PatternRow(60, 'x')], steps: 1, stepBeats: 1);
    prob.toggle(0, 0);
    prob.cells[0][0]!.probability = 0.5;
    final pp = oneClip(Clip('p', 0, 400, prob));
    final hits = compile(pp, 0, 400, at).where((e) => e.bytes[0] == 0x90).length;
    expect(hits, inInclusiveRange(160, 240));
    expect(compile(pp, 0, 400, at).length, compile(pp, 0, 400, at).length);
    expect(compile(pp, 0, 400, at, pass: 1).where((e) => e.bytes[0] == 0x90).length, isNot(hits));
  });

  test('MPE: overlapping notes on their own member channels, each starting from its own bend', () {
    final a = Note(0, 2, 60, expression: {
      'bend': [const Point(0.5, 0xA0000000)]
    });
    final b = Note(1, 2, 64, expression: {
      'bend': [const Point(0, 0x60000000)]
    });
    final p = oneClip(Clip('c', 0, 4, MidiContent(4, notes: [a, b])), mpe: true);
    expect(show(compile(p, 0, 4, at)), [
      '0 [225, 0, 64]', '0 [145, 60, 100]', // centred, then the note, on member channel 2
      '250 [225, 0, 80]', // its bend
      '500 [226, 0, 48]', '500 [146, 64, 100]', // the other note on channel 3, from its own bend
      '1000 [129, 60, 0]', '1500 [130, 64, 0]',
    ]);
    final members = allocateMembers([Note(0, 1, 60), Note(1, 1, 62), Note(0.5, 1, 64)], members: 2);
    expect(members.values.toList(), [1, 2, 1]); // 62 takes the channel free longest (1, after 60 ended)
    // the same clip on a track that is not MPE: one channel, no expression
    final plain = oneClip(Clip('c', 0, 4, MidiContent(4, notes: [a, b])), channel: 4);
    expect(compile(plain, 0, 4, at).every((e) => e.bytes[0] & 0x0F == 4 && e.bytes[0] & 0xF0 != 0xE0), isTrue);
  });

  test('mute and solo; notes end at the cut (the loop end)', () {
    final c1 = Clip('a', 0, 4, MidiContent(4, notes: [Note(0, 4, 60)]));
    final c2 = Clip('b', 0, 4, MidiContent(4, notes: [Note(0, 1, 70)]));
    final p = Project(tracks: [Track('a', port: 'a', clips: [c1]), Track('b', port: 'b', clips: [c2])]);
    expect(compile(p, 0, 4, at).map((e) => e.port).toSet(), {'a', 'b'});
    p.tracks[1].solo = true;
    expect(compile(p, 0, 4, at).map((e) => e.port).toSet(), {'b'});
    p.tracks[1].solo = false;
    p.tracks[0].mute = true;
    expect(compile(p, 0, 4, at).map((e) => e.port).toSet(), {'b'});
    p.tracks[0].mute = false;
    final off = compile(p, 0, 2, at, cut: 2).firstWhere((e) => e.port == 'a' && e.bytes[0] == 0x80);
    expect(off.tNs, at(2));
  });

  test('take in a loop: each pass adds to the clip, wrapped; held notes run to now', () {
    final t = Track('synth', port: 's');
    final p = Project(loopOn: true, loopStart: 4, loopEnd: 8, tracks: [t]);
    final take = Take.start(p, t, 4);
    expect((take.clip.start, take.clip.length), (4.0, 4.0));
    take.add(4.5, [0x90, 60, 100]);
    take.add(5.0, [0x80, 60, 0]);
    take.add(5.0, [0xB0, 1, 127]);
    take.add(9.5, [0x90, 64, 90]); // the second pass: 9.5 is 1.5 into the loop
    take.flush(10);
    final m = take.clip.content as MidiContent;
    expect([for (final n in m.notes) (n.start, n.length, n.pitch)], [(0.5, 0.5, 60), (1.5, 0.5, 64)]);
    expect(m.lanes['cc1']!.single.t, 1.0);
    expect(down16to7(m.notes[1].velocity), 90);
    take.add(11, [0x80, 64, 0]);
    take.finish(11.2);
    expect((take.clip.content as MidiContent).notes[1].length, 1.5);
    // the next take in the same loop goes into the same clip, on top of what is there
    final again = Take.start(p, t, 4);
    expect(identical(again.clip, take.clip), isTrue);
    again.add(6, [0x90, 67, 100]);
    again.finish(7);
    expect((again.clip.content as MidiContent).notes.length, 3);
  });

  test('take without a loop: a clip from the bar that grows a bar at a time', () {
    final t = Track('synth', port: 's');
    final p = Project(tracks: [t]);
    final take = Take.start(p, t, 5.5);
    expect(take.clip.start, 4);
    take.add(6, [0x90, 60, 100]);
    take.flush(6.5);
    expect(take.clip.length, 4);
    take.add(9, [0x80, 60, 0]);
    take.finish(9.1);
    expect(take.clip.length, 8);
    expect((take.clip.content as MidiContent).notes.single.length, 3);
  });

  test('an MPE take: notes keep their own bends (and the one sent just before them); the track turns MPE', () {
    final t = Track('strips', port: 's');
    final take = Take.start(Project(tracks: [t]), t, 0);
    for (final (i, ch) in [(0, 1), (1, 2), (2, 3)]) {
      take.add(i * 1.0, [0xE0 | ch, 0, 72]); // the strips send the bend first
      take.add(i * 1.0, [0x90 | ch, 60 + i, 100]);
      take.add(i * 1.0 + 0.5, [0xE0 | ch, 0, 80]);
      take.add(i * 1.0 + 0.9, [0x80 | ch, 60 + i, 0]);
    }
    take.finish(3);
    expect(t.mpe, isTrue);
    final m = take.clip.content as MidiContent;
    expect(m.notes.length, 3);
    final bend = m.notes[1].expression['bend']!;
    expect([for (final pt in bend) (pt.t, down32to14(pt.v) >> 7)], [(0.0, 72), (0.5, 80)]);
    expect(m.lanes, isEmpty);
  });

  test('undo and redo put the project back, ids and all', () {
    final c = Clip('a', 0, 4, MidiContent(4, notes: [Note(0, 1, 60)]));
    final p = Project(tracks: [Track('t', clips: [c])]);
    final h = History(p);
    h.mark();
    p.tracks.single.clips.single.start = 8;
    (p.tracks.single.clips.single.content as MidiContent).notes.single.pitch = 61;
    h.mark();
    p.tracks.add(Track('u'));
    expect(h.undo(), isTrue);
    expect(p.tracks.length, 1);
    expect(h.undo(), isTrue);
    final back = p.tracks.single.clips.single;
    expect((back.start, back.id, (back.content as MidiContent).notes.single.pitch), (0.0, c.id, 60));
    expect(h.undo(), isFalse);
    h.redo();
    expect(p.tracks.single.clips.single.start, 8);
    final copy = back.copy();
    expect(copy.id, isNot(back.id));
  });
}

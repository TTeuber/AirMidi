import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/piano_roll.dart';
import 'package:airmidi/desktop/daw/record.dart';
import 'package:airmidi/desktop/daw/roll_edit.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

List<(double, double, int)> show(Iterable<Note> ns) => [
      for (final n in ns) (double.parse(n.start.toStringAsFixed(4)), double.parse(n.length.toStringAsFixed(4)), n.pitch)
    ];

void main() {
  test('move keeps the group inside the clip and the MIDI range', () {
    final a = Note(0.5, 1, 60), b = Note(2, 1, 120);
    moveNotes([a, b], -1, 10);
    expect(show([a, b]), [(0.0, 1.0, 67), (1.5, 1.0, 127)]); // stopped at beat 0 and at 127, shape kept
  });

  test('resize and trim', () {
    final a = Note(1, 1, 60);
    resizeNotes([a], -2);
    expect(a.length, 1 / 64);
    final b = Note(1, 2, 60);
    trimStarts([b], 0.5);
    expect(show([b]), [(1.5, 1.5, 60)]);
    trimStarts([b], -5);
    expect(show([b]), [(0.0, 3.0, 60)]);
  });

  test('quantize: onto the grid keeping lengths, half way at 50 %, ends too, triplets', () {
    final a = Note(0.1, 0.4, 60), b = Note(0.9, 0.3, 62);
    quantize([a, b], 0.25);
    expect(show([a, b]), [(0.0, 0.4, 60), (1.0, 0.3, 62)]);
    final c = Note(0.1, 1, 60);
    quantize([c], 0.5, strength: 0.5);
    expect(c.start, closeTo(0.05, 1e-9));
    final d = Note(0.1, 0.8, 60);
    quantize([d], 0.25, ends: true); // 0.1..0.9 -> 0..1
    expect(show([d]), [(0.0, 1.0, 60)]);
    final e = Note(0.05, 0.05, 60);
    quantize([e], 0.25, ends: true); // both ends onto 0: one step long
    expect(show([e]), [(0.0, 0.25, 60)]);
    final t = Note(0.3, 0.1, 60);
    quantize([t], 1 / 3);
    expect(t.start, closeTo(1 / 3, 1e-9));
  });

  test('legato: each note to the next start; a chord stays a chord', () {
    final ns = [Note(0, 0.1, 60), Note(0, 0.2, 64), Note(1, 0.1, 62), Note(3, 0.5, 65)];
    legato(ns);
    expect(show(ns), [(0.0, 1.0, 60), (0.0, 1.0, 64), (1.0, 2.0, 62), (3.0, 0.5, 65)]);
  });

  test('velocity, box, paste, duplicate', () {
    final a = Note(0, 1, 60, velocity: up7to16(120));
    nudgeVelocity([a], 20);
    expect(down16to7(a.velocity), 127);
    final ns = [Note(0, 1, 60), Note(1, 1, 64), Note(2, 1, 67)];
    expect(notesInBox(ns, 0.5, 1.5, 59, 64).map((n) => n.pitch).toSet(), {60, 64});
    expect(show(pasted(ns.sublist(1), 4)), [(4.0, 1.0, 64), (5.0, 1.0, 67)]);
    expect(show(duplicated(ns.sublist(0, 2), 1)), [(2.0, 1.0, 60), (3.0, 1.0, 64)]);
    expect(show(duplicated([Note(0, 0.6, 60)], 1)), [(1.0, 0.6, 60)]); // the span rounds up to the grid
  });

  test('replace: a take in the loop starts from an empty clip; undo brings the old notes back', () {
    final t = Track('synth', port: 's');
    final p = Project(loopOn: true, loopStart: 0, loopEnd: 4, tracks: [t]);
    final first = Take.start(p, t, 0)
      ..add(0.5, [0x90, 60, 100])
      ..add(1, [0x80, 60, 0]);
    first.finish(2);
    final h = History(p)..mark();
    final again = Take.start(p, t, 0, replace: true)
      ..add(2, [0x90, 67, 100])
      ..add(3, [0x80, 67, 0]);
    again.finish(4);
    expect((t.clips.single.content as MidiContent).notes.map((n) => n.pitch), [67]);
    h.undo();
    expect((p.tracks.single.clips.single.content as MidiContent).notes.map((n) => n.pitch), [60]);
  });

  group('the piano roll widget', () {
    late MidiContent m;
    late Track track;
    late History history;
    final sounds = <String>[];
    final key = GlobalKey<PianoRollState>();

    Future<Rect> pump(WidgetTester t) async {
      t.view.physicalSize = const Size(1200, 600);
      t.view.devicePixelRatio = 1;
      m = MidiContent(4, notes: [Note(0, 1, 60)]);
      track = Track('synth', port: 'p');
      history = History(Project(tracks: [track..clips.add(Clip('c', 0, 4, m))]));
      sounds.clear();
      await t.pumpWidget(MaterialApp(
        home: Scaffold(
          body: SizedBox(
            width: 1200,
            height: 500,
            child: PianoRoll(
              key: key,
              content: m,
              track: track,
              history: history,
              changed: () {},
              audition: (p, on, v) => sounds.add('${on ? 'on' : 'off'} $p'),
            ),
          ),
        ),
      ));
      await t.pump();
      // the note grid: right of the keys, under the bar, above the velocity lane
      return const Rect.fromLTRB(64, 34, 1200, 500 - 72 - 28);
    }

    testWidgets('double-click draws a note on the grid; drag moves it a beat and up a row; delete removes it',
        (t) async {
      final g = await pump(t);
      final s = key.currentState!;
      final at = Offset(g.left + s.x(2.1), g.top + s.y(64) + 3);
      await t.tapAt(at);
      await t.pump(const Duration(milliseconds: 50));
      await t.tapAt(at);
      await t.pump();
      expect(show(m.notes), [(0.0, 1.0, 60), (2.0, 0.25, 64)]); // 2.1 snapped down to the sixteenth: 2.0
      expect(sounds.first, 'on 64');
      // drag it a beat right and a row up
      final from = Offset(g.left + s.x(2.1), g.top + s.y(64) + 3);
      final drag = await t.startGesture(from, kind: PointerDeviceKind.mouse);
      await drag.moveTo(from + Offset(s.ppb * 1.0, -s.rowH));
      await drag.up();
      await t.pump(const Duration(milliseconds: 400));
      expect(show(m.notes), [(0.0, 1.0, 60), (3.0, 0.25, 65)]);
      expect(
          s.key(const KeyDownEvent(
              physicalKey: PhysicalKeyboardKey.backspace,
              logicalKey: LogicalKeyboardKey.backspace,
              timeStamp: Duration.zero)),
          isTrue);
      expect(show(m.notes), [(0.0, 1.0, 60)]);
      history.undo(); // the project holds copies after undo; the delete is undone there
      expect((history.project.tracks.single.clips.single.content as MidiContent).notes.length, 2);
    });

    testWidgets('right edge resizes; a box selects; the velocity lane sets a selected group together', (t) async {
      final g = await pump(t);
      final s = key.currentState!;
      // the first note's right edge from beat 1 to 2
      final edge = Offset(g.left + s.x(1) - 2, g.top + s.y(60) + 3);
      final d = await t.startGesture(edge, kind: PointerDeviceKind.mouse);
      await d.moveTo(edge + Offset(s.ppb, 0));
      await d.up();
      await t.pump();
      expect(show(m.notes), [(0.0, 2.0, 60)]);
      m.notes.add(Note(2, 1, 62));
      // a box over both from empty space above them
      final a = Offset(g.left + s.x(0) + 1, g.top + s.y(66)), b = Offset(g.left + s.x(3) - 1, g.top + s.y(59));
      final box = await t.startGesture(a, kind: PointerDeviceKind.mouse);
      await box.moveTo(b);
      await box.up();
      await t.pump();
      expect(s.selected.length, 2);
      // the velocity lane: the first stalk dragged to the bottom takes both down by as much
      final lane = Offset(g.left + s.x(0), 500 - 72 + 2.0);
      final v = await t.startGesture(lane, kind: PointerDeviceKind.mouse);
      await v.moveTo(Offset(lane.dx, 500 + 20)); // past the lane's bottom: the lowest velocity
      await v.up();
      await t.pump();
      expect(m.notes.map((n) => down16to7(n.velocity)).toList(), [1, 1]);
    });

    testWidgets('a controller lane: add points, drag one, make it glide, remove it', (t) async {
      await pump(t);
      final s = key.currentState!;
      s.showLane('cc74');
      await t.pump();
      final top = t.getTopLeft(find.byKey(const ValueKey('lane')));
      Offset at(double beat, double f) => top + Offset(s.x(beat), 4 + (1 - f) * (72 - 8));
      await t.tapAt(at(0, 0.25));
      await t.pump(const Duration(milliseconds: 400));
      await t.tapAt(at(2, 1));
      await t.pump(const Duration(milliseconds: 400));
      final pts = m.lanes['cc74']!;
      expect([for (final p in pts) (p.t, down32to7(p.v))], [(0.0, 32), (2.0, 127)]);
      // drag the first up to the middle and a beat on
      final d = await t.startGesture(at(0, 0.25), kind: PointerDeviceKind.mouse);
      await d.moveTo(at(1, 0.5));
      await d.up();
      await t.pump(const Duration(milliseconds: 400));
      expect([for (final p in pts) (p.t, down32to7(p.v))], [(1.0, 64), (2.0, 127)]);
      // a double-click makes it glide to the next; a right-click removes the last
      await t.tapAt(at(1, 0.5));
      await t.pump(const Duration(milliseconds: 50));
      await t.tapAt(at(1, 0.5));
      await t.pump(const Duration(milliseconds: 400));
      expect(pts.first.ramp, isTrue);
      final r = await t.startGesture(at(2, 1), kind: PointerDeviceKind.mouse, buttons: kSecondaryMouseButton);
      await r.up();
      await t.pump();
      expect(pts.length, 1);
    });

    testWidgets("an MPE note's own bend: points relative to the note, played on its channel", (t) async {
      await pump(t);
      track.mpe = true;
      final s = key.currentState!;
      final n = m.notes.single; // 0..1, c4
      s.setSelected({n});
      s.showLane('note:bend');
      await t.pump();
      expect(s.lanePoints!.$2, 0.0); // the note's start
      final top = t.getTopLeft(find.byKey(const ValueKey('lane')));
      await t.tapAt(top + Offset(s.x(0.5), 4 + (1 - 0.75) * (72 - 8)));
      await t.pump(const Duration(milliseconds: 400));
      final bend = n.expression['bend']!.single;
      expect(bend.t, 0.5);
      expect(down32to14(bend.v), closeTo(16383 * 0.75, 64));
      // played: centred at the note on, then the bend half a beat in, on the note's member channel
      final ev = compile(Project(tracks: [track]), 0, 4, Timeline(0, 120).nsAt);
      sortEvents(ev);
      final bends = [
        for (final e in ev)
          if (e.bytes[0] & 0xF0 == 0xE0) e
      ];
      expect(bends.length, 2);
      expect(bends.map((e) => e.bytes[0]).toSet(), {0xE1});
      expect(bends[1].tNs, 250000000);
    });
  });
}

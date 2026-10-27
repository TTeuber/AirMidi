import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/pattern_editor.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart' hide Step;
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('to MIDI clip: the same notes at the same times as the pattern plays them on its first pass', () {
    final pat = Pattern(gmDrumRows(), swing: 0.6);
    for (final s in [0, 3, 6, 9, 13]) {
      pat.toggle(5, s);
    }
    pat.toggle(2, 2);
    pat.cells[5][3]!.nudge = 0.2;
    pat.cells[5][6]!.repeat = 3;
    pat.cells[5][9]!.probability = 0.5;
    pat.cells[5][13]!.velocity = up7to16(40);
    pat.cells[2][2]!.gate = 1.5;
    final clip = Clip('p', 0, 4, pat);
    final p = Project(tracks: [
      Track('d', port: 'x', channel: 9, clips: [clip])
    ]);
    final tl = Timeline(0, 120);
    final played = compile(p, 0, 4, tl.nsAt);
    sortEvents(played);
    final m = patternToMidi(pat, clip.id);
    clip.content = m;
    final again = compile(p, 0, 4, tl.nsAt);
    sortEvents(again);
    expect(again.map((e) => '$e').toList(), played.map((e) => '$e').toList());
    expect(m.length, 4);
  });

  test('a step nudged before the start wraps to the end of the MIDI clip', () {
    final pat = Pattern([PatternRow(60, 'c')]);
    pat.toggle(0, 0);
    pat.cells[0][0]!.nudge = -0.2;
    final m = patternToMidi(pat, 1);
    expect(m.notes.single.start, closeTo(4 - 0.05, 1e-9));
  });

  test('scale rows, highest on top; rotate; double', () {
    expect(scaleRows(0, 'minor', 3, 1).map((r) => r.note).toList(), [60, 58, 56, 55, 53, 51, 50, 48]);
    expect(scaleRows(9, 'pentatonic minor', 2, 2).length, 11); // a2: five notes, two octaves, and the top a
    final pat = Pattern([PatternRow(60, 'c')], steps: 4);
    pat.toggle(0, 0);
    pat.toggle(0, 3);
    rotateSteps(pat, 1);
    expect([for (var s = 0; s < 4; s++) pat.cells[0][s] != null], [true, true, false, false]); // 3 came round
    doublePattern(pat);
    expect(pat.steps, 8);
    expect([for (var s = 0; s < 8; s++) pat.cells[0][s] != null], [true, true, false, false, true, true, false, false]);
    pat.cells[0][4]!.velocity = 1;
    expect(pat.cells[0][0]!.velocity, isNot(1)); // the copies are their own
  });

  group('the sequencer widget', () {
    late Pattern pat;
    late History history;
    final sounds = <int>[];
    final key = GlobalKey<PatternEditorState>();

    Future<void> pump(WidgetTester t, {int steps = 16, double width = 1000}) async {
      t.view.physicalSize = Size(width, 500);
      t.view.devicePixelRatio = 1;
      pat = Pattern(gmDrumRows(), steps: steps);
      final track = Track('drums', port: 'd', channel: 9, clips: [Clip('beat', 0, 4, pat)]);
      history = History(Project(tracks: [track]));
      sounds.clear();
      await t.pumpWidget(MaterialApp(
        home: Scaffold(
          body: SizedBox(
            width: width,
            height: 400,
            child: PatternEditor(
              key: key,
              pattern: pat,
              track: track,
              history: history,
              changed: () {},
              audition: (p, on, v) {
                if (on) sounds.add(p);
              },
            ),
          ),
        ),
      ));
      await t.pump();
    }

    // the centre of a cell, in the grid's own coordinates plus where the grid sits on screen
    Offset cell(WidgetTester t, int r, int s) {
      final st = key.currentState!;
      final grid = t.getTopLeft(find.byKey(const ValueKey('grid')));
      return grid + Offset(st.cellX(s) + st.cellW / 2, (r - st.topRow) * st.rowH + st.rowH / 2);
    }

    testWidgets('a click adds a step and plays it, a drag paints, a click on a step removes it', (t) async {
      await pump(t);
      final kick = pat.rows.indexWhere((r) => r.name == 'kick');
      await t.tapAt(cell(t, kick, 0));
      await t.pump();
      expect(pat.cells[kick][0], isNotNull);
      expect(sounds, [36]);
      // a fast drag: one move from step 0 to step 3 still paints 1 and 2
      final g = await t.startGesture(cell(t, 2, 0), kind: PointerDeviceKind.mouse);
      await g.moveTo(cell(t, 2, 3));
      await g.up();
      await t.pump();
      expect([for (var s = 0; s < 5; s++) pat.cells[2][s] != null], [true, true, true, true, false]);
      // sideways from a step: erases along the row
      final er = await t.startGesture(cell(t, 2, 3), kind: PointerDeviceKind.mouse);
      await er.moveTo(cell(t, 2, 1));
      await er.up();
      await t.pump();
      expect([for (var s = 0; s < 5; s++) pat.cells[2][s] != null], [true, false, false, false, false]);
      await t.tapAt(cell(t, kick, 0));
      await t.pump(const Duration(milliseconds: 400));
      expect(pat.cells[kick][0], isNull);
      history.undo();
      expect((history.project.tracks.single.clips.single.content as Pattern).cells[kick][0], isNotNull);
    });

    testWidgets('dragging a step up raises its velocity; the lane sets the selected row', (t) async {
      await pump(t);
      pat.toggle(5, 4, velocity: up7to16(60));
      await t.pump();
      final g = await t.startGesture(cell(t, 5, 4), kind: PointerDeviceKind.mouse);
      await g.moveTo(cell(t, 5, 4) - const Offset(0, 30));
      await g.up();
      await t.pump();
      expect(down16to7(pat.cells[5][4]!.velocity), greaterThan(90));
      // the lane under the grid: probability of the step, row 5 selected by the drag above
      await t.tap(find.text('probability'));
      await t.pump();
      final st = key.currentState!;
      final lane = t.getBottomLeft(find.byKey(const ValueKey('lane')));
      await t.tapAt(lane + Offset(st.cellX(4) + st.cellW / 2, -72 * 0.25)); // a quarter of the way up
      await t.pump();
      expect(pat.cells[5][4]!.probability, closeTo(0.25, 0.051));
    });

    testWidgets('a lock lane: a click locks the step, a double-click takes the lock off', (t) async {
      await pump(t);
      pat.toggle(5, 4);
      await t.pump();
      await t.tap(find.text('+ lock'));
      await t.pumpAndSettle();
      await t.tap(find.text('brightness (CC 74)'));
      await t.pumpAndSettle();
      final st = key.currentState!;
      expect(st.lock, 'cc74');
      final grid = t.getTopLeft(find.byKey(const ValueKey('grid')));
      await t.tapAt(grid + Offset(20, (5 - st.topRow) * st.rowH + st.rowH / 2)); // the row's label selects it
      await t.pump(const Duration(milliseconds: 300));
      expect(st.row, 5);
      final lane = t.getBottomLeft(find.byKey(const ValueKey('lane')));
      final at = lane + Offset(st.cellX(4) + st.cellW / 2, -72 * 0.75); // three quarters up
      await t.tapAt(at);
      await t.pump(const Duration(milliseconds: 400));
      expect(down32to7(pat.cells[5][4]!.locks['cc74']!), closeTo(95, 2));
      await t
          .runAsync(() => Future.delayed(const Duration(milliseconds: 400))); // the double-click is timed by the clock
      await t.tapAt(at);
      await t.pump(const Duration(milliseconds: 50));
      await t.tapAt(at);
      await t.pump(const Duration(milliseconds: 400));
      expect(pat.cells[5][4]!.locks, isEmpty);
      expect(find.text('brightness'), findsOneWidget); // the lane stays while it is chosen
    });

    testWidgets('64 steps in a narrow panel: pages of 16 with chips', (t) async {
      await pump(t, steps: 64, width: 600);
      final st = key.currentState!;
      expect((st.paged, st.pages, st.shown), (true, 4, 16));
      await t.ensureVisible(find.text('3'));
      await t.pump();
      await t.tap(find.text('3'));
      await t.pump();
      expect(st.first, 32);
    });
  });
}

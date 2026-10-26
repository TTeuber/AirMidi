// The prototype's tests (modeling/proto/pedal_steel/test/strips.test.js), ported one for one.
import 'package:airmidi/strips/strips.dart';
import 'package:flutter_test/flutter_test.dart';

const on = EventType.on, change = EventType.change, pitch = EventType.pitch, off = EventType.off;

void main() {
  final s = LayoutSettings(row: 10, width: 20, gap: 4, tuning: defaultTuning(3));
  final L = layout(0, 100, 1000, 800, s, 1); // 1 px per mm keeps the numbers readable

  test('default tuning is guitar strings 4-3-2, then adds high E, A, low E', () {
    expect(defaultTuning(3), [50, 55, 59]);
    expect(defaultTuning(3).map(noteName), ['d3', 'g3', 'b3']);
    expect(defaultTuning(4).map(noteName), ['d3', 'g3', 'b3', 'e4']);
    expect(defaultTuning(6).map(noteName), ['e2', 'a2', 'd3', 'g3', 'b3', 'e4']);
  });

  test('row centres are exactly in tune, pitch rises going down', () {
    final st = L.strips[0];
    expect(L.pitchAt(st, 105), 50); // centre of the top row = d3
    expect(L.pitchAt(st, 115), 51);
    expect(L.pitchAt(st, 110), 50.5); // row boundary = quarter tone
    expect(L.yAt(st, L.pitchAt(st, 237)), closeTo(237, 1e-9));
    expect(L.pitchAt(L.strips[1], 105), 55); // same row on the next strip = g3
  });

  test('strips are centred in the area right of the markers; a touch in a gap goes to the nearer strip', () {
    final a = L.strips[0], b = L.strips[1];
    expect(L.areaL, 8); // 1 mm edge, 6 mm marker column, 1 mm
    expect(L.areaR, 999);
    expect(a.x, 8 + (991 - 68) / 2);
    expect(L.stripAt(a.x + a.w + 1, 1)!.i, 0);
    expect(L.stripAt(b.x - 1, 1)!.i, 1);
    expect(L.stripAt(a.x - 3, 1)!.i, 0); // a little outside the outer strips still counts
    expect(L.stripAt(10, 1), isNull);
  });

  test('nearest note and cents', () {
    expect(nearest(52.3), (note: 52, cents: 30));
    expect(nearest(51.7), (note: 52, cents: -30));
  });

  test('one note per strip: newest touch sounds, lifting falls back', () {
    final g = Strings();
    expect(g.down(1, 0, 50.2, false), const StripEvent(on, 0, 50.2));
    expect(g.down(2, 1, 55, false), const StripEvent(on, 1, 55)); // other strip: its own note
    expect(g.down(3, 0, 52, false), const StripEvent(change, 0, 52));
    expect(g.move(1, 50.4), isNull); // held but not sounding: silent
    expect(g.move(3, 52.5), const StripEvent(pitch, 0, 52.5));
    expect(g.up(3), const StripEvent(change, 0, 50.4));
    expect(g.state(0)!.pitch, 50.4);
    expect(g.state(0)!.held, isEmpty);
    expect(g.up(1), const StripEvent(off, 0));
    expect(g.state(0), isNull);
  });

  test('lifting a held, silent touch changes nothing', () {
    final g = Strings();
    g.down(1, 0, 50, false);
    g.down(2, 0, 53, false);
    expect(g.up(1), isNull);
    expect(g.state(0)!.pitch, 53);
    expect(g.state(0)!.held, isEmpty);
  });

  test('snap start lands on the nearest note, then moves continuously from there', () {
    final g = Strings();
    expect(g.down(1, 0, 52.3, true).pitch, 52);
    expect(g.move(1, 54.3)!.pitch, closeTo(54, 1e-9)); // a 2-semitone slide is still exactly 2 semitones
    expect(g.move(1, 53.0)!.pitch, closeTo(52.7, 1e-9));
  });

  test('fret marker column sits left of the first strip, clear of it', () {
    expect(L.markerX, 4); // the centre of the 6 mm column after the 1 mm edge
    final fit = layout(0, 0, 100, 800, LayoutSettings(row: 10, gap: 0, tuning: defaultTuning(6)), 1);
    expect(L.markerX + 3, lessThanOrEqualTo(fit.strips[0].x));
  });

  test('fret marks repeat every 12 frets, none on the open string', () {
    expect([for (var f = 0; f <= 24; f++) fretMarks(f)].join(), '0001010101002001010101002');
    expect(fretMarks(-12), 2);
    expect(fretMarks(-3), 1); // = fret 9 an octave down
    expect(fretMarks(-4), 0);
  });

  group('move mode', () {
    final six = LayoutSettings(row: 10, gap: 0, tuning: defaultTuning(6));
    final F = layout(0, 100, 100, 200, six, 1); // area 8..99 = 91 px, 20 rows

    test('fit: six strips with no gap fill the area exactly', () {
      expect(F.fitWidth, closeTo(91 / 6, 1e-9));
      expect(F.strips.first.x, 8);
      expect(F.strips.last.x + F.strips.last.w, closeTo(99, 1e-9));
      expect(F.overflows, isFalse);
    });

    test('the view moves every strip up the neck', () {
      final up = layout(0, 100, 100, 200, six.copyWith(view: 5), 1);
      expect(up.pitchAt(up.strips[0], 105), 45); // e2 + 5 = a2 at the top row
      expect(up.yAt(up.strips[0], 45), 105);
    });

    test('dragging up by two rows shows two semitones higher', () {
      final m = moved(F, six, const PinchStep(50, 500, 50, 480, 1, 1));
      expect(m.view, closeTo(2, 1e-9));
      expect(m.row, 10);
    });

    test('the view stops where the rows stop being MIDI notes', () {
      expect(moved(F, six, const PinchStep(50, 100, 50, 9000, 1, 1)).view, -40); // the low e strip can go down to note 0
      final high = moved(F, six, const PinchStep(50, 900, 50, -9000, 1, 1)).view;
      expect(64 + high + 20 - 0.5, closeTo(127, 1e-9)); // the bottom row of the high e strip is g9
    });

    test('a vertical pinch zooms the rows around the fingers', () {
      final m = moved(F, six, const PinchStep(50, 300, 50, 300, 1, 2));
      expect(m.row, 20);
      final Z = layout(0, 100, 100, 200, m, 1);
      expect(Z.pitchAt(Z.strips[0], 300), closeTo(F.pitchAt(F.strips[0], 300), 1e-9));
    });

    test('a horizontal pinch widens the strips; wider than the screen, they scroll around the fingers', () {
      final m = moved(F, six, const PinchStep(30, 300, 30, 300, 2, 1));
      expect(m.width, closeTo(91 / 6 * 2, 1e-9));
      final W = layout(0, 100, 100, 200, m, 1);
      expect(W.overflows, isTrue);
      final before = (30 - F.strips[0].x) / F.strips[0].w, after = (30 - W.strips[0].x) / W.strips[0].w;
      expect(after, closeTo(before, 1e-9)); // the same point across the strips stays under the fingers
      final dragged = moved(W, m, const PinchStep(50, 300, 10, 300, 1, 1));
      expect(dragged.scroll, closeTo(m.scroll + 40, 1e-9));
      final far = moved(W, m, const PinchStep(50, 300, -900, 300, 1, 1));
      expect(far.scroll, closeTo(W.total - 91, 1e-9)); // stops at the last strip
    });

    test('a touch outside a scrolled area plays the strip at the edge, not a hidden one', () {
      final W = layout(0, 100, 100, 200, six.copyWith(width: () => 30, scroll: 20), 1);
      expect(W.stripAt(3)!.i, W.stripAt(8)!.i);
      expect(W.strips[W.stripAt(3)!.i].x + W.strips[W.stripAt(3)!.i].w, greaterThan(8));
    });

    test('settled: whole semitones, and nearly fitting strips go back to fit', () {
      final s1 = settled(F, six.copyWith(view: 2.4, width: () => 91 / 6 + 0.5));
      expect(s1.view, 2);
      expect(s1.width, isNull);
      expect(settled(F, six.copyWith(view: 2.6, width: () => 25)).width, 25);
    });

    test('pinch: one finger drags, two side by side scale across only', () {
      final p = Pinch(20);
      p.down(1, 50, 500);
      final d = p.move(1, 50, 480)!;
      expect((d.fy, d.ny, d.sx, d.sy), (500, 480, 1, 1));
      p.down(2, 90, 500); // lands: no step, the centre jumps without anything moving
      final s1 = p.move(2, 130, 500)!;
      expect(s1.sx, closeTo(80 / 40, 1e-9));
      expect(s1.sy, 1); // no vertical spread
      expect((s1.fx, s1.nx), (70, 90));
      p.up(1);
      expect(p.move(2, 130, 490)!.fy, 500); // after a lift, steps start from the finger that is left
    });
  });

  test('key guide: g major keeps g a b c d e f#, greys the rest', () {
    const g = 7;
    final inG = [for (var m = 0; m < 12; m++) if (inKey(60 + m, g, 'maj')) noteName(60 + m).substring(0, noteName(60 + m).length - 1)];
    expect(inG, ['c', 'd', 'e', 'f#', 'g', 'a', 'b']);
    expect(inKey(61, g, 'off'), isTrue); // no key: everything is in
    expect(isRoot(43, g) && !isRoot(45, g), isTrue);
  });

  test('soften: flat around notes, continuous and monotone across the row boundary', () {
    expect(soften(52.3, 1), closeTo(52.3, 1e-9)); // k = 1 changes nothing
    expect(soften(52.25, 8), closeTo(52, 0.003));
    expect((soften(52.4999, 8) - soften(52.5001, 8)).abs(), lessThan(0.01)); // no jump at the boundary
    var prev = double.negativeInfinity;
    for (var x = 51.5; x <= 53.5; x += 0.01) {
      final y = soften(x, 5);
      expect(y, greaterThanOrEqualTo(prev));
      prev = y;
    }
  });

  void run(Strings g, double secs, [void Function()? f]) {
    for (var t = 0.0; t < secs; t += 1 / 60) {
      f?.call();
      g.tick(1 / 60);
    }
  }

  test('assist: a resting finger is pulled into tune over the pull time', () {
    final g = Strings()..assist = const Assist(1, 0.1);
    expect(g.down(1, 0, 52.3, false).pitch, 52.3); // lands where the finger is
    run(g, 0.05);
    final mid = g.state(0)!.pitch;
    expect(mid < 52.3 && mid > 52.05, isTrue, reason: 'half way after 50 ms ($mid)');
    run(g, 0.6);
    expect(g.state(0)!.pitch, closeTo(52, 0.005), reason: 'in tune after a rest');
  });

  test('assist: a sliding finger is followed exactly, and pulled in where it stops', () {
    final g = Strings()..assist = const Assist(1, 0.1);
    g.down(1, 0, 50, false);
    run(g, 0.5);
    var raw = 50.0;
    run(g, 0.3, () {
      raw += 0.1;
      g.move(1, raw);
    }); // 6 semitones/s
    expect(g.state(0)!.pitch, closeTo(raw, 0.03), reason: 'slide follows the finger');
    run(g, 0.8);
    expect(g.state(0)!.pitch, closeTo(raw.roundToDouble(), 0.005), reason: 'pulled onto the nearest note once still');
  });

  test('assist off: pitch is the finger, always', () {
    final g = Strings();
    g.down(1, 0, 52.3, false);
    run(g, 1);
    expect(g.state(0)!.pitch, 52.3);
  });
}

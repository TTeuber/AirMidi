// The prototype's tests (modeling/proto/pedal_steel/test/strips.test.js), ported one for one.
import 'package:airmidi/strips/strips.dart';
import 'package:flutter_test/flutter_test.dart';

const on = EventType.on, change = EventType.change, pitch = EventType.pitch, off = EventType.off;

void main() {
  final s = LayoutSettings(row: 10, width: 20, gap: 4, align: StripAlign.mid, tuning: defaultTuning(3));
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

  test('strips are centred; a touch in a gap goes to the nearer strip', () {
    final a = L.strips[0], b = L.strips[1];
    expect(a.x, (1000 - 68) / 2);
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
    expect(L.markerX, L.strips[0].x - 4 - 3); // gap 4 mm, then half the 6 mm column
    final left = layout(0, 0, 1000, 800,
        LayoutSettings(row: 10, width: 20, gap: 4, align: StripAlign.left, tuning: defaultTuning(3)), 1);
    expect(left.markerX - 3, greaterThanOrEqualTo(8)); // still inside the 8 mm margin when left-aligned
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

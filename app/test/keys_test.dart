import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/strips/strips.dart' show PinchStep;
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  const a = PhysicalKeyboardKey.keyA, w = PhysicalKeyboardKey.keyW, k = PhysicalKeyboardKey.keyK;

  test('home row from c4: A = 60, W = c#, K = the next c', () {
    final keys = Keys();
    expect(keys.keyDown(a), [
      [0x90, 60, 100]
    ]);
    expect(keys.keyDown(w), [
      [0x90, 61, 100]
    ]);
    expect(keys.keyDown(k), [
      [0x90, 72, 100]
    ]);
    expect(keys.sounding, {60, 61, 72});
    expect(keys.keyUp(a), [
      [0x80, 60, 0]
    ]);
  });

  test('octave and velocity keys; a held note ends where it started', () {
    final keys = Keys();
    keys.keyDown(a); // 60
    keys.keyDown(PhysicalKeyboardKey.keyX); // up an octave
    expect(keys.keyUp(a), [
      [0x80, 60, 0]
    ]);
    keys.keyDown(PhysicalKeyboardKey.keyV);
    expect(keys.keyDown(a), [
      [0x90, 72, 120]
    ]);
    for (var i = 0; i < 20; i++) {
      keys.keyDown(PhysicalKeyboardKey.keyX);
      keys.keyDown(PhysicalKeyboardKey.keyV);
    }
    expect([keys.octave, keys.velocity], [8, 127]);
    for (var i = 0; i < 20; i++) {
      keys.keyDown(PhysicalKeyboardKey.keyZ);
      keys.keyDown(PhysicalKeyboardKey.keyC);
    }
    expect([keys.octave, keys.velocity], [-1, 1]);
    expect(keys.keyDown(PhysicalKeyboardKey.quote), [
      [0x90, 17, 1]
    ]);
  });

  test('two sources on one note: it ends when the last lets go', () {
    final keys = Keys();
    keys.keyDown(a);
    keys.noteOn('mouse', 60);
    expect(keys.keyUp(a), isEmpty);
    expect(keys.noteOff('mouse'), [
      [0x80, 60, 0]
    ]);
  });

  test('the mouse sliding across the piano: one note at a time', () {
    final keys = Keys();
    keys.noteOn('mouse', 60);
    expect(keys.noteOn('mouse', 62), [
      [0x80, 60, 0],
      [0x90, 62, 100]
    ]);
  });

  test('sustain on Tab, once per change; all off ends notes and pedal', () {
    final keys = Keys()..channel = 2;
    expect(keys.keyDown(PhysicalKeyboardKey.tab), [
      [0xB2, 64, 127]
    ]);
    expect(keys.keyDown(PhysicalKeyboardKey.tab), isEmpty);
    keys.keyDown(a);
    keys.keyDown(k);
    expect(keys.allOff(), [
      [0x82, 60, 0],
      [0x82, 72, 0],
      [0xB2, 64, 0]
    ]);
    expect(keys.sounding, isEmpty);
  });

  test('a note ends on the channel it started on; all off covers pads', () {
    final keys = Keys();
    keys.noteOn(('pad', 0, 0), 36, ch: 9, vel: 90);
    keys.keyDown(a);
    keys.channel = 3;
    expect(keys.keyUp(a), [
      [0x80, 60, 0]
    ]);
    expect(keys.allOff(), [
      [0x89, 36, 0]
    ]);
  });

  test('unknown keys do nothing', () {
    expect(Keys().keyDown(PhysicalKeyboardKey.keyQ), isEmpty);
    expect(Keys().keyUp(PhysicalKeyboardKey.keyQ), isEmpty);
  });

  test('pads: GM drums from the bottom left, velocity by height', () {
    final p = Pads();
    expect([p.note(3, 0), p.note(3, 3), p.note(0, 0), p.note(0, 3)], [36, 39, 48, 51]);
    expect([Pads.velocityAt(0), Pads.velocityAt(1), Pads.velocityAt(0.5)], [127, 30, 79]);
  });

  test('bend: centre, ends', () {
    expect(bendAt(0, 0), [0xE0, 0, 64]);
    expect(bendAt(1, 1), [0xE1, 127, 127]);
    expect(bendAt(0, -1), [0xE0, 1, 0]);
    expect(noteName(60), 'c4');
    expect(isBlack(61), isTrue);
  });

  test('phone keys: one keyboard scrolled by white keys; velocity by where the key is struck', () {
    expect([whiteIndex(0), whiteIndex(60), whiteIndex(61), whiteIndex(127)], [0, 35, 35, 74]);
    expect([whiteNote(35), whiteNote(36), whiteNote(74)], [60, 62, 127]);
    final g = PianoView(35, 80, 800, 300); // c4 at the left edge, ten white keys of 80 px
    expect(g.hit(10, 290)!.$1, 60);
    expect(g.hit(790, 290)!.$1, 76); // e5, the tenth white key
    expect(g.hit(80 * 0.68 + 20, 20)!.$1, 61); // a black key, near its top
    expect((g.lo, g.hi), (60, 77)); // e5 and the f just past it (its edge is on screen)
    final soft = g.hit(10, 5)!.$2, hard = g.hit(10, 295)!.$2;
    expect(soft < 50 && hard > 120, isTrue);
    expect(g.hit(10, 300), isNull);
    final half = PianoView(35.5, 80, 800, 300); // half a key scrolled: c4's right half at the left
    expect(half.hit(10, 290)!.$1, 60);
    expect(half.hit(50, 290)!.$1, 62);
  });

  test('phone keys move mode: a drag scrolls, a pinch keeps the key under the fingers', () {
    // one finger 160 px left: two white keys of 80 px (10 mm at 8 px/mm) up
    var (v, w) = movedKeys(35, 10, 800, 8, const PinchStep(400, 100, 240, 100, 1, 1));
    expect((v, w), (37.0, 10.0));
    // the fingers spread to twice as far apart round x = 400: keys twice as wide, the key at 400 stays there
    (v, w) = movedKeys(35, 10, 800, 8, const PinchStep(400, 100, 400, 100, 2, 1));
    expect(w, 20.0);
    expect(v + 400 / (20 * 8), 35 + 400 / (10 * 8));
    // clamped at the ends and to the width range
    expect(movedKeys(1, 10, 800, 8, const PinchStep(0, 0, 800, 0, 1, 1)).$1, 0.0);
    expect(movedKeys(70, 10, 800, 8, const PinchStep(800, 0, 0, 0, 1, 1)).$1, 75 - 800 / 80);
    expect(movedKeys(35, 10, 800, 8, const PinchStep(400, 0, 400, 0, 9, 1)).$2, kKeyWidthRange.$2);
  });
}

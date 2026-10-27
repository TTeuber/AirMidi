import 'package:airmidi/shared/play/keys.dart';
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

  test('phone keys: two octaves, the higher on top; velocity by where the key is struck', () {
    final g = KeyRows(60, 700, 400); // rows of 200 px, 8 white keys of 87.5 px each
    expect(g.hit(10, 390)!.$1, 60); // bottom row, c4
    expect(g.hit(10, 190)!.$1, 72); // top row, c5
    expect(g.hit(690, 390)!.$1, 72); // the bottom row's last key is the next c
    expect(g.hit(87.5 * 0.68 + 20, 210)!.$1, 61); // a black key, near its top
    final soft = g.hit(10, 205)!.$2, hard = g.hit(10, 395)!.$2;
    expect(soft < 50 && hard > 120, isTrue);
    expect(g.hit(10, 400), isNull);
  });
}

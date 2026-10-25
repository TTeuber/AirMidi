import 'package:airmidi/midi/mpe.dart';
import 'package:airmidi/midi/packets.dart';
import 'package:airmidi/strips/strips.dart';
import 'package:flutter_test/flutter_test.dart';

const on = EventType.on, change = EventType.change, pitch = EventType.pitch, off = EventType.off;

double bendOf(List<int> m) => ((m[2] << 7 | m[1]) - 8192) / 8192 * kBendRange;

void main() {
  test('setup: MPE configuration on the manager channel, ±48 on each member', () {
    final m = mpeSetup(3);
    expect(m.take(6), [
      [0xB0, 101, 0], [0xB0, 100, 6], [0xB0, 6, 3], [0xB0, 38, 0], [0xB0, 101, 127], [0xB0, 100, 127]
    ]);
    expect(m.length, 6 * 4);
    expect(m[6 + 2], [0xB1, 6, 48]); // strip 0 = channel 2
    expect(m[18 + 2], [0xB3, 6, 48]); // strip 2 = channel 4
  });

  test('bend resolution: 14 bits over ±48 semitones', () {
    expect(bendMessage(1, 0), [0xE1, 0, 64]);
    expect(bendOf(bendMessage(1, 0.3)), closeTo(0.3, 0.003)); // 0.6 cents per step
    expect(bendMessage(1, 60), [0xE1, 127, 127]); // clamped
  });

  test('touch: bend before note on, on the strip\'s own channel', () {
    final o = MpeOut(legato: () => true);
    o.handle(const StripEvent(on, 1, 55.2));
    final m = o.take();
    expect(m.length, 2);
    expect(m[0][0], 0xE2);
    expect(bendOf(m[0]), closeTo(0.2, 0.003));
    expect(m[1], [0x92, 55, kVelocity]);
  });

  test('moves coalesce to the newest bend; tiny moves are not sent', () {
    final o = MpeOut(legato: () => true);
    o.handle(const StripEvent(on, 0, 50));
    o.take();
    o.handle(const StripEvent(pitch, 0, 50.001));
    expect(o.hasDue, isFalse); // a tenth of a cent
    for (final p in [50.1, 50.2, 50.3, 52.0]) {
      o.handle(StripEvent(pitch, 0, p));
    }
    final m = o.take();
    expect(m.length, 1);
    expect(bendOf(m[0]), closeTo(2.0, 0.003));
    expect(o.coalesced, 3);
  });

  test('note messages keep their order and the bend after them is the newest', () {
    final o = MpeOut(legato: () => false);
    o.handle(const StripEvent(on, 0, 50));
    o.handle(const StripEvent(pitch, 0, 50.5));
    o.handle(const StripEvent(change, 0, 53)); // legato off: off + on
    o.handle(const StripEvent(pitch, 0, 53.4));
    final m = o.take();
    expect(m.map((x) => x[0]).toList(), [0xE1, 0x91, 0x81, 0xE1, 0x91, 0xE1]);
    expect(m[2], [0x81, 50, 0]);
    expect(m[4], [0x91, 53, kVelocity]);
    expect(bendOf(m[5]), closeTo(0.4, 0.003));
  });

  test('legato: a second finger on a sounding strip only bends', () {
    final o = MpeOut(legato: () => true);
    o.handle(const StripEvent(on, 0, 50));
    o.take();
    o.handle(const StripEvent(change, 0, 52));
    final m = o.take();
    expect(m.length, 1);
    expect(bendOf(m[0]), closeTo(2, 0.003));
  });

  test('two strips on the same note stay apart', () {
    final o = MpeOut(legato: () => true);
    o.handle(const StripEvent(on, 0, 55)); // d strip, 5th fret
    o.handle(const StripEvent(on, 1, 55)); // open g
    o.handle(const StripEvent(off, 0));
    final m = o.take();
    expect(m.where((x) => x[0] & 0xF0 == 0x90).map((x) => x[0]), [0x91, 0x92]);
    expect(m.last, [0x81, 55, 0]);
  });

  test('all off', () {
    final o = MpeOut(legato: () => true);
    o.handle(const StripEvent(on, 0, 50));
    o.handle(const StripEvent(on, 2, 59));
    o.take();
    expect(o.allOff(), [
      [0x81, 50, 0],
      [0x83, 59, 0]
    ]);
    expect(o.allOff(), isEmpty);
  });

  test('BLE MIDI packets: header, a timestamp per message, split at the MTU', () {
    final p = blePackets([
      [0xE1, 0, 64],
      [0x91, 50, 100]
    ], 0x1234, 20);
    // 0x1234 & 0x1FFF = 0x1234: high 6 bits 0x24, low 7 bits 0x34
    expect(p.single, [0xA4, 0xB4, 0xE1, 0, 64, 0xB4, 0x91, 50, 100]);
    final many = blePackets(List.generate(10, (i) => [0xE1, i, 64]), 0, 20); // 4 bytes each, 19 usable
    expect(many.length, 3);
    expect(many.every((x) => x.length <= 20), isTrue);
    expect(many.fold<int>(0, (n, x) => n + (x.length - 1) ~/ 4), 10);
  });

  test('UMP: MIDI 1.0 channel voice words, big-endian', () {
    expect(umpWords([
      [0x91, 50, 100],
      [0xE1, 0, 64]
    ]), [0x20, 0x91, 50, 100, 0x20, 0xE1, 0, 64]);
  });
}

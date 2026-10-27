import 'package:airmidi/tracks/sequencer.dart';
import 'package:flutter_test/flutter_test.dart';

const ms = 1000000;

void main() {
  test('timeline: 120 bpm is 500 ms a beat; a tempo change keeps the beats before it', () {
    final tl = Timeline(1000 * ms, 120);
    expect(tl.nsAt(1), 1500 * ms);
    expect(tl.beatAt(2000 * ms), 2);
    tl.setBpm(60, 2000 * ms);
    expect(tl.beatAt(2000 * ms), 2);
    expect(tl.nsAt(3), 3000 * ms);
  });

  test('a four-on-the-floor kick: note on every beat, off half a step later', () {
    final t = drumTrack(port: 'p');
    final kick = t.rows.indexWhere((r) => r.label == 'kick');
    for (final s in [0, 4, 8, 12]) {
      t.toggle(kick, s);
    }
    final song = Song(tracks: [t]);
    final ev = eventsBetween(song, Timeline(0, 120), 0, 2);
    expect(ev.map((e) => '${e.tNs ~/ ms} ${e.bytes}').toList(), [
      '0 [153, 36, 100]',
      '62 [137, 36, 0]', // half a sixteenth at 120 bpm: 62.5 ms
      '500 [153, 36, 100]',
      '562 [137, 36, 0]',
    ]);
  });

  test('windows join without gaps or doubles', () {
    final t = drumTrack(port: 'p');
    for (var s = 0; s < 16; s++) {
      t.toggle(2, s);
    }
    final song = Song(tracks: [t]);
    final tl = Timeline(0, 133);
    final all = eventsBetween(song, tl, 0, 4);
    final pieces = <SeqEvent>[];
    var from = 0.0;
    for (final to in [0.13, 0.5, 0.51, 1.7, 2.0, 3.99, 4.0]) {
      pieces.addAll(eventsBetween(song, tl, from, to));
      from = to;
    }
    expect(pieces.map((e) => '$e').toList(), all.map((e) => '$e').toList());
    expect(all.where((e) => e.bytes[0] == 0x99).length, 16);
  });

  test('tracks of different lengths loop on their own; mute and no port send nothing', () {
    final a = drumTrack(port: 'a')..steps = 3, b = drumTrack(port: 'b'), c = drumTrack();
    a.toggle(5, 0);
    b.toggle(5, 0);
    c.toggle(5, 0);
    final song = Song(tracks: [a, b, c]);
    final on = eventsBetween(song, Timeline(0, 120), 0, 4).where((e) => e.bytes[0] == 0x99);
    expect(on.where((e) => e.port == 'a').length, 6); // steps 0 3 6 9 12 15
    expect(on.where((e) => e.port == 'b').length, 1);
    b.mute = true;
    expect(eventsBetween(song, Timeline(0, 120), 0, 4).where((e) => e.port == 'b'), isEmpty);
  });

  test('a gate of a whole step: the off of one note comes before the next on', () {
    final t = noteTrack(port: 'p')..gate = 1;
    t.toggle(0, 0);
    t.toggle(0, 1);
    final ev = eventsBetween(Song(tracks: [t]), Timeline(0, 120), 0, 0.5);
    expect(ev.map((e) => e.bytes[0]).toList(), [0x90, 0x80, 0x90, 0x80]);
  });

  test('clock: 24 ticks a beat to the clock ports', () {
    final song = Song()..clockPorts.add('drum machine');
    final ev = eventsBetween(song, Timeline(0, 120), 0, 1);
    expect(ev.length, 24);
    expect(ev[1].tNs, closeTo(500 * ms / 24, 1));
    expect(ev.every((e) => e.bytes.single == 0xF8 && e.port == 'drum machine'), isTrue);
  });

  test('the scheduled notes are reported, for stopping', () {
    final t = drumTrack(port: 'p')..toggle(5, 0);
    final notes = <ScheduledNote>[];
    eventsBetween(Song(tracks: [t]), Timeline(0, 120), 0, 1, notes: notes);
    expect(notes.single.note, 36);
    expect(notes.single.offNs, 62500000);
  });

  test('the playhead', () {
    final t = drumTrack()..steps = 12;
    expect([stepAt(t, 4, 0), stepAt(t, 4, 0.99), stepAt(t, 4, 3), stepAt(t, 4, -0.1)], [0, 3, 0, -1]);
  });
}

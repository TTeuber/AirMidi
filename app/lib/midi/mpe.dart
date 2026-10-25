// Strip events -> MPE (MIDI 1.0), paced by the link.
//
// MPE lower zone: manager channel 1 (index 0), strip n on member channel 2 + n (index 1 + n). Each strip's note
// starts on the nearest semitone and every movement after that is pitch bend on its own channel, ±48 semitones
// (the MPE default for member channels): 0.6 cents per step of the 14-bit bend.
//
// Pacing: note ons and offs are queued in order and never dropped; pitch bends are coalesced to the newest value
// per channel. take() hands out everything due since the last call, and the sender calls it again only once the
// link has taken the previous batch. A link that drains slower than the finger moves then sends fewer, newer bends
// instead of building a queue, which is what makes a slide lag behind the finger.
import '../strips/strips.dart';

const kBendRange = 48; // semitones each way, member channels
const kVelocity = 100;
const _minBendStep = 0.005; // semitones (0.5 cent): smaller changes are not sent

int memberChannel(int strip) => 1 + strip; // 0-based channel index

/// The MPE Configuration Message for a lower zone of [members] channels, then pitch bend range ±48 on each
/// member channel, then the null RPN so a stray data entry changes nothing.
List<List<int>> mpeSetup(int members) {
  List<List<int>> rpn(int ch, int lsb, int value) => [
        [0xB0 | ch, 101, 0],
        [0xB0 | ch, 100, lsb],
        [0xB0 | ch, 6, value],
        [0xB0 | ch, 38, 0],
        [0xB0 | ch, 101, 127],
        [0xB0 | ch, 100, 127],
      ];
  return [
    ...rpn(0, 6, members),
    for (var s = 0; s < members; s++) ...rpn(memberChannel(s), 0, kBendRange),
  ];
}

List<int> bendMessage(int ch, double semitones) {
  final v = (8192 + semitones / kBendRange * 8192).round().clamp(0, 16383);
  return [0xE0 | ch, v & 0x7F, v >> 7];
}

class _Voice {
  int? note; // sounding note, null when silent
  double sentBend = 0; // last bend handed out, semitones
  double? dueBend; // newest bend not handed out yet
}

class MpeOut {
  final bool Function() legato;
  final _queue = <List<int>>[];
  final _voices = <int, _Voice>{};
  int coalesced = 0; // bends replaced by a newer one before they were sent (for the stats)

  MpeOut({required this.legato});

  _Voice _voice(int strip) => _voices.putIfAbsent(strip, _Voice.new);

  bool get hasDue => _queue.isNotEmpty || _voices.values.any((v) => v.dueBend != null);
  bool get sounding => _voices.values.any((v) => v.note != null);

  void handle(StripEvent? e) {
    if (e == null) return;
    final v = _voice(e.strip), ch = memberChannel(e.strip);
    switch (e.type) {
      case EventType.on:
        _start(v, ch, e.pitch);
      case EventType.change:
        final note = v.note;
        if (note != null && legato() && (e.pitch - note).abs() <= kBendRange) {
          _bend(v, e.pitch - note);
        } else {
          _stop(v, ch);
          _start(v, ch, e.pitch);
        }
      case EventType.pitch:
        final note = v.note;
        if (note != null) _bend(v, (e.pitch - note).clamp(-kBendRange, kBendRange).toDouble());
      case EventType.off:
        _stop(v, ch);
    }
  }

  // The bend goes out before the note on, so the note starts at its pitch.
  void _start(_Voice v, int ch, double pitch) {
    final note = pitch.round().clamp(0, 127);
    v.note = note;
    v.dueBend = null;
    v.sentBend = pitch - note;
    _queue.add(bendMessage(ch, v.sentBend));
    _queue.add([0x90 | ch, note, kVelocity]);
  }

  void _stop(_Voice v, int ch) {
    final note = v.note;
    if (note == null) return;
    v.dueBend = null;
    v.note = null;
    _queue.add([0x80 | ch, note, 0]);
  }

  void _bend(_Voice v, double semis) {
    if (v.dueBend != null) {
      coalesced++;
    } else if ((semis - v.sentBend).abs() < _minBendStep) {
      return;
    }
    v.dueBend = semis;
  }

  /// Everything due, in order: the queued note messages, then the newest bend per channel.
  List<List<int>> take() {
    final out = List.of(_queue);
    _queue.clear();
    _voices.forEach((strip, v) {
      final b = v.dueBend;
      if (b == null) return;
      out.add(bendMessage(memberChannel(strip), b));
      v.sentBend = b;
      v.dueBend = null;
    });
    return out;
  }

  /// Note off for everything sounding, and forget the voices.
  List<List<int>> allOff() {
    for (final e in _voices.entries) {
      _stop(e.value, memberChannel(e.key));
    }
    return take();
  }
}

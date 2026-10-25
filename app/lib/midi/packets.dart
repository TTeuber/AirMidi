// MIDI 1.0 messages -> BLE MIDI packets, and -> UMP words for the box's WebSocket.
import 'dart:typed_data';

/// BLE MIDI 1.0 packets (Apple / MMA spec): a header byte with the timestamp's high 6 bits, then per message a
/// timestamp byte with its low 7 bits and the full message (no running status, so every message stands alone).
/// Messages are packed into as few packets as fit in [maxLen] bytes (the ATT MTU minus 3).
List<Uint8List> blePackets(List<List<int>> msgs, int timeMs, int maxLen) {
  final ts = timeMs & 0x1FFF;
  final header = 0x80 | (ts >> 7), stamp = 0x80 | (ts & 0x7F);
  final packets = <Uint8List>[];
  var cur = <int>[header];
  for (final m in msgs) {
    if (cur.length + 1 + m.length > maxLen && cur.length > 1) {
      packets.add(Uint8List.fromList(cur));
      cur = [header];
    }
    cur
      ..add(stamp)
      ..addAll(m);
  }
  if (cur.length > 1) packets.add(Uint8List.fromList(cur));
  return packets;
}

/// MIDI 1.0 channel voice messages as UMP type 2 (MIDI 1.0 channel voice, group 0), one 32-bit word each,
/// big-endian: the box's /ws format.
Uint8List umpWords(List<List<int>> msgs) {
  final b = ByteData(msgs.length * 4);
  for (var i = 0; i < msgs.length; i++) {
    final m = msgs[i];
    b.setUint32(i * 4, 0x20000000 | m[0] << 16 | (m.length > 1 ? m[1] : 0) << 8 | (m.length > 2 ? m[2] : 0));
  }
  return b.buffer.asUint8List();
}

/// A BLE MIDI packet's MIDI bytes: the header and each timestamp byte left out (as the firmware's blemidi_decode).
/// Empty if it is not a BLE MIDI packet.
List<int> bleMidiBytes(List<int> pkt) {
  if (pkt.isEmpty || pkt[0] & 0xC0 != 0x80) return const [];
  final out = <int>[];
  var afterTs = false;
  for (var i = 1; i < pkt.length; i++) {
    final b = pkt[i];
    if (b & 0x80 != 0 && !afterTs) {
      afterTs = true; // timestamp low byte
      continue;
    }
    afterTs = false;
    out.add(b);
  }
  return out;
}

/// MIDI 1.0 bytes in, channel and system common messages out (running status kept across calls, real-time bytes and
/// SysEx left out: the app reads controls, not dumps).
class Midi1Parser {
  int _status = 0;
  final _data = <int>[];
  bool _sysex = false;

  static int _length(int status) {
    final t = status & 0xF0;
    if (t == 0xC0 || t == 0xD0) return 1;
    if (t < 0xF0) return 2;
    return switch (status) { 0xF1 || 0xF3 => 1, 0xF2 => 2, _ => 0 };
  }

  List<List<int>> feed(List<int> bytes) {
    final out = <List<int>>[];
    for (final b in bytes) {
      if (b >= 0xF8) continue; // real time, may sit anywhere
      if (b == 0xF0) {
        _sysex = true;
        continue;
      }
      if (b == 0xF7) {
        _sysex = false;
        continue;
      }
      if (b & 0x80 != 0) {
        _sysex = false;
        _status = b;
        _data.clear();
        if (_length(b) == 0) {
          out.add([b]);
          _status = 0;
        }
        continue;
      }
      if (_sysex || _status == 0) continue;
      _data.add(b);
      if (_data.length == _length(_status)) {
        out.add([_status, ..._data]);
        _data.clear();
        if (_status >= 0xF0) _status = 0; // system common cancels running status
      }
    }
    return out;
  }
}

/// UMP words from the box's WebSocket (big-endian) -> MIDI 1.0 channel voice messages: MIDI 1.0 channel voice (type
/// 2) as they are, MIDI 2.0 channel voice (type 4) scaled down for CC, pitch bend and channel pressure.
List<List<int>> umpToMidi1(List<int> bytes) {
  final out = <List<int>>[];
  final b = ByteData.sublistView(Uint8List.fromList(bytes));
  var i = 0;
  while (i + 4 <= bytes.length) {
    final w = b.getUint32(i);
    final mt = w >> 28;
    final words = const [1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4][mt];
    if (i + 4 * words > bytes.length) break;
    final status = (w >> 16) & 0xFF, i1 = (w >> 8) & 0x7F, i2 = w & 0x7F;
    if (mt == 2) {
      final len = Midi1Parser._length(status);
      out.add([status, if (len > 0) i1, if (len > 1) i2]);
    } else if (mt == 4) {
      final data = b.getUint32(i + 4);
      switch (status & 0xF0) {
        case 0xB0:
          out.add([status, i1, data >> 25]);
        case 0xD0:
          out.add([status, data >> 25]);
        case 0xE0:
          final v = data >> 18;
          out.add([status, v & 0x7F, v >> 7]);
        case 0x90 || 0x80:
          out.add([status, i1, (data >> 25).clamp(status & 0xF0 == 0x90 ? 1 : 0, 127)]);
      }
    }
    i += 4 * words;
  }
  return out;
}

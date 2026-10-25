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

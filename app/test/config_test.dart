// The config service's framing, the same cases as the firmware's test/test_blecfg.c.
import 'dart:convert';
import 'dart:typed_data';

import 'package:airmidi/shared/link/config.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a short request is one piece with the last flag', () {
    final p = splitMessage(7, utf8.encode('GET /api/status\n'), 20);
    expect(p.length, 1);
    expect(p[0].sublist(0, 2), [7, kLast]);
  });

  test('a long request splits at the MTU and reassembles', () {
    final msg = utf8.encode('POST /api/wifi/try\n{"ssid":"home","pass":"correct horse"}');
    final pieces = splitMessage(1, msg, 8);
    expect(pieces.length, (msg.length + 5) ~/ 6);
    expect(pieces.every((x) => x.length <= 8), isTrue);
    expect(pieces.where((x) => x[1] & kLast != 0).length, 1);
    final r = Reassembler();
    Uint8List? whole;
    for (final p in pieces) {
      whole = r.add(p);
    }
    expect(whole, msg);
  });

  test('an empty message is a header alone', () {
    final p = splitMessage(3, [], 20);
    expect(p.single, [3, kLast]);
  });

  test('replies to different ids reassemble apart', () {
    final r = Reassembler();
    expect(r.add(Uint8List.fromList([1, 0, ...utf8.encode('20')])), isNull);
    expect(r.add(Uint8List.fromList([2, kLast, ...utf8.encode('404\n{}')])), utf8.encode('404\n{}'));
    expect(r.add(Uint8List.fromList([1, kLast, ...utf8.encode('0\n{"a":1}')])), utf8.encode('200\n{"a":1}'));
  });

  test('parse a reply', () {
    final (s, j) = parseReply(utf8.encode('200\n{"mode":"play"}'));
    expect(s, 200);
    expect(j['mode'], 'play');
    final (s2, j2) = parseReply(utf8.encode('202\n'));
    expect(s2, 202);
    expect(j2, isEmpty);
  });
}

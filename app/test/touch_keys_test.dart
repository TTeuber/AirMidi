import 'package:airmidi/mobile/pads_page.dart';
import 'package:airmidi/mobile/touch_keys_page.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

class FakeLink implements MidiLink {
  final sent = <List<int>>[];
  @override
  String get label => 'fake';
  @override
  Stream<List<int>> get incoming => const Stream.empty();
  @override
  bool get repeatSetup => false;
  @override
  Future<void> send(List<List<int>> msgs) async => sent.addAll(msgs);
  @override
  Future<void> close() async {}
}

List<String> notes(FakeLink l) => [
      for (final m in l.sent)
        if ((m[0] & 0xE0) == 0x80) '${m[0] & 0xF0 == 0x90 ? 'on' : 'off'} ${m[1]}'
    ];

void main() {
  testWidgets('keys: a finger sliding across plays each key it reaches; two fingers, two notes', (t) async {
    t.view.physicalSize = const Size(400, 800);
    t.view.devicePixelRatio = 1;
    final link = FakeLink();
    await t.pumpWidget(MaterialApp(home: TouchKeysPage(link: link)));
    await t.pumpAndSettle();
    final area = t.getRect(find.byType(CustomPaint).last);
    final w = area.width / 8; // white keys per row
    final y = area.bottom - 20; // the bottom row, the white keys' front
    final g = await t.startGesture(Offset(area.left + w * 0.5, y));
    await t.pump();
    for (var k = 1; k < 5; k++) {
      await g.moveTo(Offset(area.left + w * (k + 0.5), y));
      await t.pump();
    }
    final g2 = await t.startGesture(Offset(area.left + w * 0.5, area.top + area.height / 2 - 20)); // top row c5
    await t.pump();
    await g.up();
    await g2.up();
    await t.pump();
    expect(notes(link), [
      'on 60', 'off 60', 'on 62', 'off 62', 'on 64', 'off 64', 'on 65', 'off 65', 'on 67', //
      'on 72', 'off 67', 'off 72',
    ]);
  });

  testWidgets('pads: velocity from where the finger lands, kick bottom left on channel 10', (t) async {
    t.view.physicalSize = const Size(400, 800);
    t.view.devicePixelRatio = 1;
    final link = FakeLink();
    await t.pumpWidget(MaterialApp(home: PadsPage(link: link)));
    await t.pumpAndSettle();
    final grid = t.getRect(find.byKey(const ValueKey('pads')));
    final side = (grid.width - 24) / 4;
    await t.tapAt(Offset(grid.left + side / 2, grid.bottom - side + 4)); // kick, near its top: loud
    await t.tapAt(Offset(grid.left + side / 2, grid.bottom - 4)); // kick, near its bottom: soft
    await t.pump();
    final ons = [for (final m in link.sent) if (m[0] == 0x99) m];
    expect(ons.map((m) => m[1]), [36, 36]);
    expect(ons[0][2] > 120 && ons[1][2] < 40, isTrue);
  });
}

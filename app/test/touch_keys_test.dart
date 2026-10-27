import 'package:airmidi/mobile/pads_page.dart';
import 'package:airmidi/mobile/touch_keys_page.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/strips/strips.dart' show kPxPerMm;
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
    t.view.physicalSize = const Size(900, 400);
    t.view.devicePixelRatio = 1;
    final link = FakeLink();
    await t.pumpWidget(MaterialApp(home: TouchKeysPage(link: link)));
    await t.pumpAndSettle();
    final area = t.getRect(find.byType(CustomPaint).last);
    final w = 14 * kPxPerMm; // a white key, c4 at the left edge
    final y = area.bottom - 20; // the white keys' front
    final g = await t.startGesture(Offset(area.left + w * 0.5, y));
    await t.pump();
    for (var k = 1; k < 5; k++) {
      await g.moveTo(Offset(area.left + w * (k + 0.5), y));
      await t.pump();
    }
    final g2 = await t.startGesture(Offset(area.left + w * 7.5, y)); // c5
    await t.pump();
    await g.up();
    await g2.up();
    await t.pump();
    expect(notes(link), [
      'on 60', 'off 60', 'on 62', 'off 62', 'on 64', 'off 64', 'on 65', 'off 65', 'on 67', //
      'on 72', 'off 67', 'off 72',
    ]);
  });

  testWidgets('keys move mode: a drag scrolls and plays nothing, a pinch widens the keys', (t) async {
    t.view.physicalSize = const Size(900, 400);
    t.view.devicePixelRatio = 1;
    final link = FakeLink(), view = KeysView();
    await t.pumpWidget(MaterialApp(home: TouchKeysPage(link: link, view: view)));
    await t.pumpAndSettle();
    await t.tap(find.text('move'));
    await t.pumpAndSettle();
    link.sent.clear();
    final area = t.getRect(find.byType(CustomPaint).last);
    final w = 14 * kPxPerMm, y = area.center.dy;
    // drag left by two and a bit keys: the view lands on a whole key once the finger lifts
    await t.dragFrom(Offset(area.center.dx, y), Offset(-w * 2.2, 0));
    await t.pumpAndSettle();
    expect(view.view, 37.0);
    expect(notes(link), isEmpty);
    // two fingers spreading apart across: wider keys
    final a = await t.startGesture(Offset(area.center.dx - 100, y));
    final b = await t.startGesture(Offset(area.center.dx + 100, y));
    for (var k = 1; k <= 5; k++) {
      await a.moveTo(Offset(area.center.dx - 100 - 10.0 * k, y));
      await b.moveTo(Offset(area.center.dx + 100 + 10.0 * k, y));
      await t.pump();
    }
    await a.up();
    await b.up();
    await t.pumpAndSettle();
    expect(view.width, closeTo(14 * 300 / 200, 0.5));
    expect(view.view, view.view.roundToDouble());
    // done: playing again
    await t.tap(find.text('done'));
    await t.pumpAndSettle();
    await t.tapAt(Offset(area.left + 5, area.bottom - 20));
    await t.pump();
    expect(notes(link), ['on ${whiteNote(view.view.toInt())}', 'off ${whiteNote(view.view.toInt())}']);
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
    final ons = [
      for (final m in link.sent)
        if (m[0] == 0x99) m
    ];
    expect(ons.map((m) => m[1]), [36, 36]);
    expect(ons[0][2] > 120 && ons[1][2] < 40, isTrue);
  });
}

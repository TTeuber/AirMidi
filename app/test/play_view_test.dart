// The desktop's play page: the synth's controls in columns above the keys, and the keys still play.
import 'dart:convert';
import 'dart:io';

import 'package:airmidi/desktop/keys_page.dart';
import 'package:airmidi/shared/link/config.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/envelope_editor.dart';
import 'package:airmidi/shared/ui/synth_page.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'touch_keys_test.dart' show FakeLink, notes;

class FixtureConfig implements ConfigClient {
  final api = jsonDecode(File('test/fixtures/daisy_synth_api.json').readAsStringSync()) as Map<String, dynamic>;
  @override
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]) async =>
      path == '/api/ci/device' ? api : {'report': 'ok'};
  @override
  Future<void> close() async {}
}

void main() {
  test('columns fill in order, up to the height, and all fit', () {
    expect(fillColumns([100, 100, 100, 100], 2, 250), [0, 0, 1, 1]);
    expect(fillColumns([300, 50, 50], 3, 1000), [0, 0, 0]); // fits in one: the others stay empty
    expect(fillColumns([300, 200, 200, 200], 2, 300), [0, 0, 1, 1]); // too tall for 300: shared out
    final many = fillColumns(List.filled(10, 60.0), 3, 100);
    expect(many.last, 2);
    expect([for (var c = 0; c < 3; c++) many.where((x) => x == c).length].reduce((a, b) => a > b ? a : b) <= 4, isTrue);
  });

  testWidgets('play page: synth sections side by side over the keys; the computer keys still play', (t) async {
    t.view.physicalSize = const Size(1400, 900);
    t.view.devicePixelRatio = 1;
    final link = FakeLink(), synth = SynthState(FixtureConfig());
    await t.pumpWidget(MaterialApp(home: KeysPage(link: link, synth: synth)));
    await t.pump(const Duration(seconds: 4)); // the list, then the report's polling
    final envs = find.byType(EnvelopeEditor);
    expect(envs, findsNWidgets(2));
    expect(t.getTopLeft(find.text('fx')).dx > 300, isTrue); // fx is not in the first column
    await t.sendKeyDownEvent(LogicalKeyboardKey.keyA, physicalKey: PhysicalKeyboardKey.keyA);
    await t.sendKeyUpEvent(LogicalKeyboardKey.keyA, physicalKey: PhysicalKeyboardKey.keyA);
    await t.pump();
    expect(notes(link), ['on 60', 'off 60']);
    await t.pumpWidget(const SizedBox());
  });
}

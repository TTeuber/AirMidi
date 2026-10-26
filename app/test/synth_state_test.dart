// The synth page's values outlive the page (the bug: reopening it showed the defaults again).
import 'dart:async';

import 'package:airmidi/link/config.dart';
import 'package:airmidi/link/link.dart';
import 'package:airmidi/synth/state.dart';
import 'package:flutter_test/flutter_test.dart';

class FakeConfig implements ConfigClient {
  int reads = 0;
  @override
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]) async {
    reads++;
    return {
      'controls': [
        {'title': 'Attack', 'channel': 1, 'ctrlType': 'cc', 'ctrlIndex': [73], 'default': 0},
        {'title': 'Cutoff', 'channel': 1, 'ctrlType': 'cc', 'ctrlIndex': [74], 'default': 2147483648},
      ]
    };
  }

  @override
  Future<void> close() async {}
}

class FakeLink implements MidiLink {
  final ctl = StreamController<List<int>>.broadcast();
  @override
  Stream<List<int>> get incoming => ctl.stream;
  @override
  String get label => 'fake';
  @override
  bool get repeatSetup => false;
  @override
  Future<void> send(List<List<int>> msgs) async {}
  @override
  Future<void> close() async {}
}

void main() {
  test('a value set on the page is still there when the page opens again', () async {
    final cfg = FakeConfig(), s = SynthState(cfg);
    await s.load();
    s.device!.controls[0].value = 123456789; // the page set Attack
    await s.load(); // the page opened again
    expect(s.device!.controls[0].value, 123456789);
    expect(cfg.reads, 1);
  });

  test('a rescan reads the list again and keeps the values', () async {
    final s = SynthState(FakeConfig());
    await s.load();
    s.device!.controls[1].value = 42;
    await s.load(force: true);
    expect(s.device!.controls[1].value, 42);
  });

  test('the device\'s CCs move the values with no page open', () async {
    final s = SynthState(FakeConfig()), link = FakeLink();
    await s.load();
    s.attach(link);
    link.ctl.add([0xB0, 73, 127]);
    await Future<void>.delayed(Duration.zero);
    expect(s.device!.controls[0].value, 0xFFFFFFFF);
  });
}

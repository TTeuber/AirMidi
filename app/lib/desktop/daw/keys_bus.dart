// The app's own keys page as an input for the DAW: what it plays goes onto this bus (stamped with the engine's
// clock) instead of out to a port; an armed track with input kKeysInput records it and passes it through to its
// own output.
import 'dart:async';

import 'package:airmidi/desktop/desktop_midi.dart';
import 'package:airmidi/shared/link/link.dart';

final keysBus = StreamController<(List<int>, int)>.broadcast();

class KeysBusLink implements MidiLink {
  @override
  String get label => 'keys page · to the armed tracks';

  @override
  Stream<List<int>> get incoming => const Stream.empty();

  @override
  bool get repeatSetup => false;

  @override
  Future<void> send(List<List<int>> msgs) async {
    final t = DesktopMidi.engine.nowNs;
    for (final m in msgs) {
      keysBus.add((m, t));
    }
  }

  @override
  Future<void> close() async {}
}

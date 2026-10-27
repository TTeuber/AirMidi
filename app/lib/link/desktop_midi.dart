// The computer's MIDI ports on the desktop (packages/midi_engine, docs/desktop.md D1): a synth on USB, the IAC bus,
// another app's port, the box once macOS has it (Bluetooth in Audio MIDI Setup, or a Network MIDI session), and the
// app's own virtual ports ("airmidi app") that Logic and other apps see.
import 'dart:async';
import 'dart:io';

import 'package:midi_engine/midi_engine.dart';

import 'link.dart';

const kVirtualName = 'airmidi app';

class DesktopMidi {
  static bool get available => Platform.isMacOS || Platform.isWindows || Platform.isLinux;

  static MidiEngine get engine => MidiEngine.instance;

  static int? _virtualOut, _virtualIn;

  /// The app's own ports, made once: a source other apps receive from, and a destination they send to. Null where
  /// the system has no virtual ports.
  static (int?, int?) virtualPorts() {
    if (_virtualOut == null) {
      try {
        _virtualOut = engine.openVirtual(kVirtualName, output: true);
        _virtualIn = engine.openVirtual(kVirtualName, output: false);
      } catch (_) {}
    }
    return (_virtualOut, _virtualIn);
  }

  /// Ports we can send to, without our own.
  static List<MidiPortInfo> outputs() => engine.outputs.where((p) => p.name != kVirtualName).toList();

  static Stream<void> get changes => engine.portsChanged;

  /// A link to an output, with the input of the same name (a device's two sides) for what it sends back.
  static DesktopMidiLink open(MidiPortInfo out) {
    final h = engine.open(out);
    int? inH;
    final back = engine.inputs.where((p) => p.name == out.name);
    if (back.isNotEmpty) {
      try {
        inH = engine.open(back.first);
      } catch (_) {}
    }
    return DesktopMidiLink._(out.name, h, inH, owned: true);
  }

  /// A link to the app's own virtual port: what the strips play there, Logic records.
  static DesktopMidiLink openVirtual() {
    final (o, i) = virtualPorts();
    if (o == null) throw StateError('this system has no virtual MIDI ports');
    return DesktopMidiLink._(kVirtualName, o, i, owned: false);
  }
}

class DesktopMidiLink implements MidiLink {
  final String name;
  final int handle;
  final int? inHandle;
  final bool owned; // the virtual ports stay open for the app's life
  DesktopMidiLink._(this.name, this.handle, this.inHandle, {required this.owned});

  @override
  Stream<List<int>> get incoming =>
      DesktopMidi.engine.events.where((e) => inHandle != null && e.handle == inHandle).map((e) => e.bytes);

  @override
  String get label => '$name · midi';

  /// A computer records what it gets; a box behind macOS's MIDI would want the repeat, but cannot be told apart.
  @override
  bool get repeatSetup => false;

  @override
  Future<void> send(List<List<int>> msgs) async {
    DesktopMidi.engine.send(handle, [for (final m in msgs) ...m]);
  }

  @override
  Future<void> close() async {
    if (!owned) return;
    DesktopMidi.engine.close(handle);
    if (inHandle != null) DesktopMidi.engine.close(inHandle!);
  }
}

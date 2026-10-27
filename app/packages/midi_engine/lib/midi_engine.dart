// The desktop MIDI engine (src/engine.h) from Dart: the computer's MIDI ports, input stamped on arrival, output
// sent at the time it is due. One engine per process ([MidiEngine.instance]); it starts on first use.
import 'dart:async';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

/// A port as the computer lists it. [output]: we send to it; else we receive from it.
class MidiPortInfo {
  final int index;
  final bool output;
  final String name;
  final int type; // libremidi transport_type bits
  const MidiPortInfo(this.index, this.output, this.name, this.type);

  bool get isVirtual => type & 2 != 0; // software: another app's port, or the IAC bus
  bool get isUsb => type & 16 != 0;
  bool get isBluetooth => type & 32 != 0;
  bool get isNetwork => type & 128 != 0;

  @override
  String toString() => '${output ? 'out' : 'in'} $index $name ($type)';
}

/// A message from an input: its bytes and when it arrived on the engine's clock ([MidiEngine.nowNs]).
class MidiInEvent {
  final int handle;
  final List<int> bytes;
  final int tNs;
  const MidiInEvent(this.handle, this.bytes, this.tNs);
}

typedef _WakeC = Void Function();

class MidiEngine {
  MidiEngine._(this._lib) {
    _start = _lib.lookupFunction<Int32 Function(Pointer<NativeFunction<_WakeC>>), int Function(Pointer<NativeFunction<_WakeC>>)>('ame_start');
    _now = _lib.lookupFunction<Int64 Function(), int Function()>('ame_now_ns');
    _refresh = _lib.lookupFunction<Int32 Function(), int Function()>('ame_refresh');
    _count = _lib.lookupFunction<Int32 Function(Int32), int Function(int)>('ame_port_count');
    _name = _lib.lookupFunction<Pointer<Utf8> Function(Int32, Int32), Pointer<Utf8> Function(int, int)>('ame_port_name');
    _type = _lib.lookupFunction<Int32 Function(Int32, Int32), int Function(int, int)>('ame_port_type');
    _open = _lib.lookupFunction<Int32 Function(Int32, Int32), int Function(int, int)>('ame_open');
    _openVirtual =
        _lib.lookupFunction<Int32 Function(Int32, Pointer<Utf8>), int Function(int, Pointer<Utf8>)>('ame_open_virtual');
    _close = _lib.lookupFunction<Void Function(Int32), void Function(int)>('ame_close');
    _clear = _lib.lookupFunction<Int32 Function(Int32), int Function(int)>('ame_clear');
    _send = _lib.lookupFunction<Int32 Function(Int32, Pointer<Uint8>, Int32, Int64),
        int Function(int, Pointer<Uint8>, int, int)>('ame_send');
    _read = _lib.lookupFunction<
        Int32 Function(Pointer<Int32>, Pointer<Uint8>, Int32, Pointer<Int32>, Pointer<Int64>),
        int Function(Pointer<Int32>, Pointer<Uint8>, int, Pointer<Int32>, Pointer<Int64>)>('ame_read');
    _wake = NativeCallable<_WakeC>.listener(_drain);
    if (_start(_wake.nativeFunction) != 0) throw StateError('no MIDI system on this computer');
    _list();
  }

  static MidiEngine? _instance;
  static MidiEngine get instance => _instance ??= MidiEngine._(_load());

  static DynamicLibrary _load() {
    if (Platform.isMacOS) return DynamicLibrary.open('midi_engine.framework/midi_engine');
    if (Platform.isWindows) return DynamicLibrary.open('midi_engine.dll');
    if (Platform.isLinux) return DynamicLibrary.open('libmidi_engine.so');
    throw UnsupportedError('the MIDI engine is for desktops (Android has its own bridge)');
  }

  final DynamicLibrary _lib;
  late final int Function(Pointer<NativeFunction<_WakeC>>) _start;
  late final int Function() _now, _refresh;
  late final int Function(int) _count;
  late final Pointer<Utf8> Function(int, int) _name;
  late final int Function(int, int) _type, _open;
  late final int Function(int, Pointer<Utf8>) _openVirtual;
  late final void Function(int) _close;
  late final int Function(int) _clear;
  late final int Function(int, Pointer<Uint8>, int, int) _send;
  late final int Function(Pointer<Int32>, Pointer<Uint8>, int, Pointer<Int32>, Pointer<Int64>) _read;
  late final NativeCallable<_WakeC> _wake;

  final _events = StreamController<MidiInEvent>.broadcast();
  final _ports = StreamController<void>.broadcast();
  List<MidiPortInfo> inputs = [], outputs = [];

  /// Every message from every open input.
  Stream<MidiInEvent> get events => _events.stream;

  /// The port lists changed ([inputs] and [outputs] are already new).
  Stream<void> get portsChanged => _ports.stream;

  /// The engine's clock, in ns: arrival times, and the times [send] takes.
  int get nowNs => _now();

  void _list() {
    _refresh();
    List<MidiPortInfo> side(int out) => [
          for (var i = 0; i < _count(out); i++) MidiPortInfo(i, out == 1, _name(out, i).toDartString(), _type(out, i))
        ];
    inputs = side(0);
    outputs = side(1);
  }

  static const _cap = 4096;
  final _h = calloc<Int32>(), _n = calloc<Int32>(), _t = calloc<Int64>();
  final _buf = calloc<Uint8>(_cap);

  void _drain() {
    var ports = false;
    for (;;) {
      final kind = _read(_h, _buf, _cap, _n, _t);
      if (kind == 0) break;
      if (kind == 2) {
        ports = true;
      } else {
        _events.add(MidiInEvent(_h.value, List<int>.of(_buf.asTypedList(_n.value)), _t.value));
      }
    }
    if (ports) {
      _list();
      _ports.add(null);
    }
  }

  /// Opens a port from [inputs] or [outputs]; a handle for [send] and [close], and the key of its [events].
  int open(MidiPortInfo p) {
    final h = _open(p.output ? 1 : 0, p.index);
    if (h < 0) throw StateError('could not open ${p.name}');
    return h;
  }

  /// A port other apps see under [name]: [output] true = a source they receive from (we send), false = a
  /// destination they send to (we receive). Not every system has them (Windows needs Windows MIDI Services).
  int openVirtual(String name, {required bool output}) {
    final s = name.toNativeUtf8();
    try {
      final h = _openVirtual(output ? 1 : 0, s);
      if (h < 0) throw StateError('could not create the virtual port $name');
      return h;
    } finally {
      calloc.free(s);
    }
  }

  void close(int handle) => _close(handle);

  /// Drops what is still queued for [handle]; returns how many messages were dropped.
  int clear(int handle) => _clear(handle);

  /// Sends whole messages, now or at [atNs] (on [nowNs]'s clock); same-time messages keep their order.
  void send(int handle, List<int> bytes, {int atNs = 0}) {
    if (bytes.isEmpty) return;
    final p = calloc<Uint8>(bytes.length);
    try {
      p.asTypedList(bytes.length).setAll(0, bytes);
      if (_send(handle, p, bytes.length, atNs) != 0) throw StateError('port $handle is not open for sending');
    } finally {
      calloc.free(p);
    }
  }
}

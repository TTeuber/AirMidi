// Android's MIDI service (android/.../MainActivity.kt), for what is not an airmidi box: a Mac or any other BLE MIDI
// device (Android's driver asks for the fast connection interval itself, unlike a plain GATT client), USB MIDI devices,
// and the USB peripheral port, i.e. the phone as a USB MIDI device on a computer (USB mode "MIDI"). Plain MIDI 1.0
// both ways; no box API behind it.
import 'dart:async';

import 'package:flutter/services.dart';

import 'package:airmidi/shared/midi/packets.dart';
import 'package:airmidi/shared/link/link.dart';

class MidiDeviceEntry {
  final int id;
  final String name, type; // type: usb, virtual, other
  final int inputs, outputs;
  const MidiDeviceEntry(this.id, this.name, this.type, this.inputs, this.outputs);
}

class AndroidMidi {
  static const _calls = MethodChannel('airmidi/midi');
  static final Stream<Map> events =
      const EventChannel('airmidi/midi/events').receiveBroadcastStream().map((e) => e as Map);

  /// USB and virtual MIDI devices (Bluetooth ones come from the BLE scan).
  static Future<List<MidiDeviceEntry>> devices() async {
    final list = await _calls.invokeListMethod<Map>('devices') ?? [];
    return [
      for (final d in list)
        MidiDeviceEntry(
            d['id'] as int, d['name'] as String, d['type'] as String, d['inputs'] as int, d['outputs'] as int)
    ];
  }

  /// Fires when a MIDI device comes or goes (a USB cable, the USB mode).
  static Stream<void> get changes => events.where((e) => e['event'] == 'devices');

  static Future<AndroidMidiLink> openBluetooth(String address, String name) async {
    final r = await _calls
        .invokeMapMethod<String, dynamic>('openBluetooth', {'address': address}).timeout(const Duration(seconds: 15));
    return AndroidMidiLink._(r!['handle'] as int, name, 'ble');
  }

  static Future<AndroidMidiLink> open(MidiDeviceEntry d) async {
    final r = await _calls.invokeMapMethod<String, dynamic>('open', {'id': d.id}).timeout(const Duration(seconds: 5));
    return AndroidMidiLink._(r!['handle'] as int, d.name, d.type);
  }
}

class AndroidMidiLink implements MidiLink {
  final int handle;
  final String name, transport;
  AndroidMidiLink._(this.handle, this.name, this.transport);

  late final Stream<List<int>> _in = () {
    final parser = Midi1Parser();
    return AndroidMidi.events
        .where((e) => e['event'] == 'midi' && e['handle'] == handle)
        .expand((e) => parser.feed(e['bytes'] as Uint8List))
        .asBroadcastStream();
  }();

  @override
  Stream<List<int>> get incoming => _in;

  @override
  String get label => '$name · $transport · android midi';

  /// A computer records what it gets: no zone setup every few seconds into a take.
  @override
  bool get repeatSetup => false;

  @override
  Future<void> send(List<List<int>> msgs) => AndroidMidi._calls.invokeMethod('send', {
        'handle': handle,
        'bytes': Uint8List.fromList([for (final m in msgs) ...m])
      });

  @override
  Future<void> close() => AndroidMidi._calls.invokeMethod('close', {'handle': handle});
}

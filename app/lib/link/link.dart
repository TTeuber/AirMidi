// The ways MIDI gets to the box: its BLE MIDI characteristic (any mode with BLE on), or its WebSocket (Network
// mode). A0 only sends; settings over BLE come with the config service (plan phase A1).
import 'dart:async';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

import '../midi/packets.dart';

const kBleMidiService = '03b80e5a-ede8-4b33-a751-6ce34ec4c700';
const kBleMidiChar = '7772e5db-3868-4112-a1a9-f2669d106bf3';

abstract class MidiLink {
  String get label;

  /// Hands [msgs] to the link; completes once the link has taken them (the send loop waits for this before it
  /// takes the next batch).
  Future<void> send(List<List<int>> msgs);
  Future<void> close();
}

class BleMidiLink implements MidiLink {
  final String deviceId, name;
  int mtu = 23;
  BleMidiLink._(this.deviceId, this.name);

  @override
  String get label => '$name · ble · mtu $mtu';

  static final _clock = Stopwatch()..start();

  static Future<BleMidiLink> connect(String deviceId, String name) async {
    await UniversalBle.connect(deviceId, timeout: const Duration(seconds: 10));
    final link = BleMidiLink._(deviceId, name);
    final services = await UniversalBle.discoverServices(deviceId);
    final midi = services.where((s) => s.uuid.toLowerCase() == kBleMidiService);
    if (midi.isEmpty || !midi.first.characteristics.any((c) => c.uuid.toLowerCase() == kBleMidiChar)) {
      await UniversalBle.disconnect(deviceId);
      throw StateError('$name has no BLE MIDI characteristic');
    }
    try {
      link.mtu = await UniversalBle.requestMtu(deviceId, 247);
    } catch (_) {
      // keep 23: packets get split smaller
    }
    return link;
  }

  @override
  Future<void> send(List<List<int>> msgs) async {
    for (final p in blePackets(msgs, _clock.elapsedMilliseconds, mtu - 3)) {
      await UniversalBle.write(deviceId, kBleMidiService, kBleMidiChar, p, withoutResponse: true);
    }
  }

  @override
  Future<void> close() => UniversalBle.disconnect(deviceId);
}

class WsMidiLink implements MidiLink {
  final String host;
  final WebSocketChannel _ch;
  WsMidiLink._(this.host, this._ch);

  @override
  String get label => '$host · wi-fi';

  static Future<WsMidiLink> connect(String host) async {
    final ch = WebSocketChannel.connect(Uri.parse('ws://$host/ws'));
    await ch.ready.timeout(const Duration(seconds: 5));
    ch.stream.listen((_) {}, onError: (_) {}); // the box sends what is routed to the page; A0 ignores it
    return WsMidiLink._(host, ch);
  }

  @override
  Future<void> send(List<List<int>> msgs) async {
    _ch.sink.add(Uint8List.fromList(umpWords(msgs)));
  }

  @override
  Future<void> close() => _ch.sink.close();
}

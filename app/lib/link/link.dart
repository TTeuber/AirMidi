// The ways to reach the box. Over BLE one connection (BleBox) carries both the BLE MIDI characteristic and the config
// service (firmware/components/ble_config). Over Wi-Fi (Network mode) MIDI goes over the WebSocket.
import 'dart:async';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

import '../midi/packets.dart';
import 'config.dart';

const kBleMidiService = '03b80e5a-ede8-4b33-a751-6ce34ec4c700';
const kBleMidiChar = '7772e5db-3868-4112-a1a9-f2669d106bf3';

abstract class MidiLink {
  String get label;

  /// MIDI 1.0 messages from the box (what it routes to this phone: the device's own CCs when its knobs move).
  Stream<List<int>> get incoming;

  /// Hands [msgs] to the link; completes once the link has taken them (the send loop waits for this before it
  /// takes the next batch).
  Future<void> send(List<List<int>> msgs);
  Future<void> close();
}

/// One BLE connection to a box.
class BleBox {
  final String deviceId, name;
  int mtu = 23;
  late final midi = BleMidiLink(this);
  late final config = BleConfigClient(deviceId, () => mtu);
  BleBox._(this.deviceId, this.name);

  static Future<BleBox> connect(String deviceId, String name) async {
    await UniversalBle.connect(deviceId, timeout: const Duration(seconds: 10));
    final box = BleBox._(deviceId, name);
    final services = await UniversalBle.discoverServices(deviceId);
    final midi = services.where((s) => s.uuid.toLowerCase() == kBleMidiService);
    if (midi.isEmpty || !midi.first.characteristics.any((c) => c.uuid.toLowerCase() == kBleMidiChar)) {
      await UniversalBle.disconnect(deviceId);
      throw StateError('$name has no BLE MIDI characteristic');
    }
    box.config.available = services.any((s) => s.uuid.toLowerCase() == kConfigService);
    try {
      box.mtu = await UniversalBle.requestMtu(deviceId, 247);
    } catch (_) {
      // keep 23: packets get split smaller
    }
    return box;
  }

  Future<void> close() async {
    await config.close();
    await UniversalBle.disconnect(deviceId);
  }
}

class BleMidiLink implements MidiLink {
  final BleBox box;
  BleMidiLink(this.box);

  StreamController<List<int>>? _in;
  StreamSubscription<Uint8List>? _sub;

  // Subscribed on first use and kept: the box routes to a BLE port only while its notifications are on.
  @override
  Stream<List<int>> get incoming {
    _in ??= StreamController<List<int>>.broadcast(onListen: () {
      if (_sub != null) return;
      final parser = Midi1Parser();
      _sub = UniversalBle.characteristicValueStream(box.deviceId, kBleMidiChar).listen((p) {
        for (final m in parser.feed(bleMidiBytes(p))) {
          _in!.add(m);
        }
      });
      UniversalBle.subscribeNotifications(box.deviceId, kBleMidiService, kBleMidiChar).catchError((_) {});
    });
    return _in!.stream;
  }

  @override
  String get label => '${box.name} · ble · mtu ${box.mtu}';

  static final _clock = Stopwatch()..start();

  @override
  Future<void> send(List<List<int>> msgs) async {
    for (final p in blePackets(msgs, _clock.elapsedMilliseconds, box.mtu - 3)) {
      await UniversalBle.write(box.deviceId, kBleMidiService, kBleMidiChar, p, withoutResponse: true);
    }
  }

  @override
  Future<void> close() async {} // the box's connection outlives the strips page
}

class WsMidiLink implements MidiLink {
  final String host;
  final WebSocketChannel _ch;
  final _in = StreamController<List<int>>.broadcast();
  WsMidiLink._(this.host, this._ch);

  @override
  Stream<List<int>> get incoming => _in.stream;

  @override
  String get label => '$host · wi-fi';

  static Future<WsMidiLink> connect(String host) async {
    final ch = WebSocketChannel.connect(Uri.parse('ws://$host/ws'));
    await ch.ready.timeout(const Duration(seconds: 5));
    final link = WsMidiLink._(host, ch);
    ch.stream.listen((frame) {
      if (frame is List<int>) umpToMidi1(frame).forEach(link._in.add); // binary frames: UMP words
    }, onError: (_) {});
    return link;
  }

  @override
  Future<void> send(List<List<int>> msgs) async {
    _ch.sink.add(Uint8List.fromList(umpWords(msgs)));
  }

  @override
  Future<void> close() => _ch.sink.close();
}

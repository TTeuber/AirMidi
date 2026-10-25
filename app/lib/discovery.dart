// Boxes around: BLE advertisements (BLE MIDI service; the box's id in the scan response) and mDNS (_http._tcp with a
// TXT id), merged by the id (firmware board_id()), so one box is one entry however it can be reached. Also waits for
// a box to appear on the other transport when it changes mode (Network mode turns BLE off and Wi-Fi on).
import 'dart:async';

import 'package:bonsoir/bonsoir.dart';
import 'package:flutter/foundation.dart';
import 'package:universal_ble/universal_ble.dart';

import 'link/link.dart';

/// The id in a box's scan response: manufacturer data, company FFFF (testing), payload "am" + 3 bytes.
String? boxIdFromAdvert(BleDevice d) {
  for (final m in d.manufacturerDataList) {
    final p = m.payload;
    if (m.companyId == 0xFFFF && p.length >= 5 && p[0] == 0x61 && p[1] == 0x6D) {
      return p.sublist(2, 5).map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    }
  }
  return null;
}

final _hexId = RegExp(r'^[0-9a-f]{6}$');

class Sighting {
  final String key; // the box id, or "ble:<device id>" for a BLE MIDI device without one
  String name;
  String? bleDevice; // BLE device id while advertising
  int? rssi;
  DateTime? bleSeen;
  String? host; // IPv4 address on Wi-Fi
  Sighting(this.key, this.name);

  String? get id => _hexId.hasMatch(key) ? key : null;
  bool get onBle => bleDevice != null;
  bool get onWifi => host != null;
}

class Discovery extends ChangeNotifier {
  final boxes = <String, Sighting>{};
  StreamSubscription<BleDevice>? _ble;
  BonsoirDiscovery? _mdns;
  Timer? _prune;
  String? error;

  /// Boxes with an id first, then other BLE MIDI devices; by name within each.
  List<Sighting> get list => boxes.values.toList()
    ..sort((a, b) => a.id == null && b.id != null
        ? 1
        : a.id != null && b.id == null
            ? -1
            : a.name.compareTo(b.name));

  Future<void> start() async {
    _prune ??= Timer.periodic(const Duration(seconds: 2), (_) => _pruneBle());
    await startBle();
    if (_mdns == null) {
      try {
        final d = BonsoirDiscovery(type: '_http._tcp');
        await d.ready;
        d.eventStream!.listen(_onMdns);
        await d.start();
        _mdns = d;
      } catch (e) {
        error = 'wi-fi discovery: $e';
        notifyListeners();
      }
    }
  }

  /// BLE scanning stops while connecting (Android prefers that); this starts it again.
  Future<void> startBle() async {
    try {
      await UniversalBle.requestPermissions();
      _ble ??= UniversalBle.scanStream.listen(_onBle);
      await UniversalBle.startScan(scanFilter: ScanFilter(withServices: [kBleMidiService]));
    } catch (e) {
      error = 'bluetooth: $e';
      notifyListeners();
    }
  }

  Future<void> stopBle() => UniversalBle.stopScan();

  void _onBle(BleDevice d) {
    final id = boxIdFromAdvert(d);
    final key = id ?? 'ble:${d.deviceId}';
    if (id != null) boxes.remove('ble:${d.deviceId}'); // its id arrived with a later scan response
    final s = boxes.putIfAbsent(key, () => Sighting(key, d.name ?? d.deviceId));
    if (d.name?.isNotEmpty == true) s.name = d.name!;
    s.bleDevice = d.deviceId;
    s.rssi = d.rssi;
    s.bleSeen = DateTime.now();
    notifyListeners();
  }

  void _onMdns(BonsoirDiscoveryEvent e) {
    final svc = e.service;
    if (svc == null) return;
    if (e.type == BonsoirDiscoveryEventType.discoveryServiceFound) {
      svc.resolve(_mdns!.serviceResolver);
    } else if (e.type == BonsoirDiscoveryEventType.discoveryServiceResolved && svc is ResolvedBonsoirService) {
      final id = svc.attributes['id'];
      if (id == null || !_hexId.hasMatch(id) || svc.host == null) return; // not an airmidi box
      final s = boxes.putIfAbsent(id, () => Sighting(id, svc.name));
      s.host = svc.host!.replaceAll(RegExp(r'\.$'), '');
      if (!s.onBle) s.name = svc.name;
      notifyListeners();
    } else if (e.type == BonsoirDiscoveryEventType.discoveryServiceLost) {
      for (final s in boxes.values.where((s) => s.name == svc.name && s.onWifi).toList()) {
        s.host = null;
        if (!s.onBle) boxes.remove(s.key);
      }
      notifyListeners();
    }
  }

  // A box that stopped advertising (connected elsewhere, Network mode, off) is no longer on BLE after 8 s.
  void _pruneBle() {
    final old = DateTime.now().subtract(const Duration(seconds: 8));
    var changed = false;
    for (final s in boxes.values.toList()) {
      if (s.bleSeen != null && s.bleSeen!.isBefore(old)) {
        s.bleDevice = null;
        s.bleSeen = null;
        if (!s.onWifi) boxes.remove(s.key);
        changed = true;
      }
    }
    if (changed) notifyListeners();
  }

  /// Wait (up to [timeout]) for the box with [id] to be reachable over BLE ([ble]) or on Wi-Fi. For BLE only an
  /// advertisement heard after [since] counts: one from before a restart would send us to a box that is going down.
  Future<Sighting?> waitFor(String id,
      {required bool ble, DateTime? since, Duration timeout = const Duration(seconds: 40)}) async {
    bool ready() {
      final s = boxes[id];
      if (s == null) return false;
      if (!ble) return s.onWifi;
      return s.onBle && (since == null || s.bleSeen!.isAfter(since));
    }

    if (ready()) return boxes[id];
    final done = Completer<Sighting?>();
    void check() {
      if (!done.isCompleted && ready()) done.complete(boxes[id]);
    }

    addListener(check);
    if (ble) await startBle();
    try {
      return await done.future.timeout(timeout, onTimeout: () => null);
    } finally {
      removeListener(check);
    }
  }

  @override
  void dispose() {
    _prune?.cancel();
    _ble?.cancel();
    _mdns?.stop();
    super.dispose();
  }
}

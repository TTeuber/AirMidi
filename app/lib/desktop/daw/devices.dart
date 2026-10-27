// A track's synth (docs/daw.md A5, the device tab): its MIDI-CI control list, fetched once from a box (over Wi-Fi
// or BLE: /api/ci/device, what the box read from the synth on its TRS jacks) or from a file, and kept with the
// project. With it the piano roll names the lanes, and the bottom panel's device tab shows the synth's controls
// (the play view's SynthControls) and sends their CCs through the track's own port, following the synth's knobs on
// the input of the same name. The values start at the list's defaults: asking the synth where its knobs are needs
// the box (that comes with device state, A6).
import 'dart:async';

import 'package:midi_engine/midi_engine.dart';

import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/player.dart';
import 'package:airmidi/desktop/desktop_midi.dart' show kVirtualName;
import 'package:airmidi/shared/discovery.dart';
import 'package:airmidi/shared/link/config.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/synth/controls.dart';
import 'package:airmidi/shared/synth/state.dart';

/// The control list a box has from its synth; throws when the box has found none.
Future<Map<String, dynamic>> fetchDeviceList(Discovery discovery, Sighting s) async {
  Map<String, dynamic> j;
  if (s.onWifi) {
    final c = HttpConfigClient(s.host!);
    try {
      j = await c.call('GET', '/api/ci/device');
    } finally {
      await c.close();
    }
  } else if (s.onBle) {
    await discovery.stopBle();
    try {
      final box = await BleBox.connect(s.bleDevice!, s.name);
      try {
        j = await box.config.call('GET', '/api/ci/device');
      } finally {
        await box.close();
      }
    } finally {
      discovery.startBle();
    }
  } else {
    throw StateError('${s.name} is not on Wi-Fi or Bluetooth now');
  }
  checkDeviceList(j);
  return j;
}

/// Throws unless [j] is a control list with controls in it.
void checkDeviceList(Map<String, dynamic> j) {
  if (Device.fromApi(j).controls.isEmpty) {
    throw StateError('no controls: the box has not found a MIDI-CI synth on its TRS jacks');
  }
}

String deviceName(Map<String, dynamic> j) {
  final n = Device.fromApi(j).name;
  return n.isEmpty ? 'synth' : n;
}

/// The saved list as a config client: the list, nothing else (no box to ask).
class ListConfig implements ConfigClient {
  final Map<String, dynamic> list;
  ListConfig(this.list);

  @override
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]) async {
    if (method == 'GET' && path == '/api/ci/device') return list;
    throw StateError('not connected to the box');
  }

  @override
  Future<void> close() async {}
}

/// A track's port as a link: sends go out through the player now, what the synth sends back comes in on the
/// input of the same name (a device's two sides).
class PortLink implements MidiLink {
  final DawPlayer player;
  final MidiEngine engine;
  final String port;
  int? _in;
  PortLink(this.player, this.engine, this.port);

  @override
  String get label => port;

  @override
  bool get repeatSetup => false;

  @override
  Stream<List<int>> get incoming {
    // the app's own port's other side is what the app itself sends: following it would fight every drag
    if (port == kVirtualName) return const Stream.empty();
    if (_in == null) {
      final p = engine.inputs.where((p) => p.name == port);
      if (p.isNotEmpty) {
        try {
          _in = engine.open(p.first);
        } catch (_) {}
      }
    }
    final h = _in;
    return h == null ? const Stream.empty() : engine.events.where((e) => e.handle == h).map((e) => e.bytes);
  }

  @override
  Future<void> send(List<List<int>> msgs) async {
    for (final m in msgs) {
      player.sendNow(port, m);
    }
  }

  @override
  Future<void> close() async {
    final h = _in;
    _in = null;
    if (h != null) engine.close(h);
  }
}

/// Each track's synth state and link, made when the device tab first shows it and remade when its list or port
/// changes.
class TrackSynths {
  final DawPlayer player;
  final MidiEngine engine;
  TrackSynths(this.player, this.engine);

  final _made = <int, (Map<String, dynamic>, String, SynthState, PortLink)>{};

  (SynthState, PortLink)? of(Track t) {
    final list = t.device, port = t.port;
    if (list == null || port == null) return null;
    final have = _made[t.id];
    if (have != null && identical(have.$1, list) && have.$2 == port) return (have.$3, have.$4);
    if (have != null) _drop(t.id);
    final synth = SynthState(ListConfig(list)), link = PortLink(player, engine, port);
    _made[t.id] = (list, port, synth, link);
    return (synth, link);
  }

  void _drop(int id) {
    final h = _made.remove(id);
    if (h == null) return;
    h.$4.close();
    h.$3.dispose();
  }

  void dispose() {
    for (final id in [..._made.keys]) {
      _drop(id);
    }
  }
}

/// The track's list as a Device, parsed once per list.
final _parsed = Expando<Device>();
Device? deviceOf(Track t) {
  final j = t.device;
  if (j == null) return null;
  return _parsed[j] ??= Device.fromApi(j);
}

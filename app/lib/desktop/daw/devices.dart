// A track's synth (docs/daw.md A5, the device tab): its MIDI-CI control list, fetched once from a box (over Wi-Fi
// or BLE: /api/ci/device, what the box read from the synth on its TRS jacks) or from a file, and kept with the
// project. With it the piano roll names the lanes, and the bottom panel's device tab shows the synth's controls
// (the play view's SynthControls) and sends their CCs through the track's own port, following the synth's knobs on
// the input of the same name.
//
// Device state (A6): what the device tab sets, and what the synth's own knobs send back, is kept on the track
// (Track.state) and goes out again when the project opens and when play starts. "read from the synth" asks the box for
// a MIDI-CI MIDI Message Report: the synth answers with its CCs, which come to the app over the box's own link (its
// Wi-Fi web socket or BLE MIDI, not the track's port), and all of them become the track's settings.
import 'dart:async';

import 'package:midi_engine/midi_engine.dart';

import 'package:airmidi/desktop/daw/lanes.dart' show laneKeyOf;
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
  final void Function()? sent; // after the controls sent something (their values moved)
  int? _in;
  PortLink(this.player, this.engine, this.port, {this.sent});

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
    sent?.call();
  }

  @override
  Future<void> close() async {
    final h = _in;
    _in = null;
    if (h != null) engine.close(h);
  }
}

/// Each track's synth state and link, made when the device tab first shows it and remade when its list or port
/// changes. The controls start where the track's saved settings have them, and whatever moves them is saved back.
class TrackSynths {
  final DawPlayer player;
  final MidiEngine engine;
  final Project project;
  TrackSynths(this.player, this.engine, this.project);

  final _made = <int, (Map<String, dynamic>, String, SynthState, PortLink)>{};

  (SynthState, PortLink)? of(Track t) {
    final list = t.device, port = t.port;
    if (list == null || port == null) return null;
    final have = _made[t.id];
    if (have != null && identical(have.$1, list) && have.$2 == port) return (have.$3, have.$4);
    if (have != null) _drop(t.id);
    final id = t.id;
    final d = Device.fromApi(list);
    for (final c in d.controls) {
      final v = t.state[laneKeyOf(c)];
      if (v != null) c.value = v;
    }
    final synth = SynthState(ListConfig(list))..device = d; // loaded: SynthControls will not read the list again
    final link = PortLink(player, engine, port, sent: () => keep(id));
    synth.addListener(() => keep(id));
    _made[id] = (list, port, synth, link);
    return (synth, link);
  }

  /// The synth's values onto its track (after undo, onto the restored one): the ones moved from the default, or
  /// saved already; [all]: every one (they were read from the synth).
  void keep(int id, {bool all = false}) {
    final d = _made[id]?.$3.device;
    final t = project.tracks.where((t) => t.id == id).firstOrNull;
    if (d == null || t == null) return;
    for (final c in d.controls) {
      final k = laneKeyOf(c);
      if (k == null) continue;
      if (all || t.state.containsKey(k) || c.value != c.def) t.state[k] = c.value;
    }
  }

  void keepAll() {
    for (final id in _made.keys) {
      keep(id);
    }
  }

  /// Asks [s] for its synth's current values (MIDI-CI MIDI Message Report) and keeps them on [t]. Returns the
  /// report's outcome ("ok", "not supported", ...) and how many values arrived (changed or not).
  Future<(String, int)> readCurrent(Track t, Discovery discovery, Sighting s) async {
    final got = of(t);
    if (got == null) throw StateError('${t.name} has no synth controls or no port');
    final synth = got.$1;
    MidiLink via;
    ConfigClient config;
    BleBox? ble;
    if (s.onWifi) {
      via = await WsMidiLink.connect(s.host!);
      config = HttpConfigClient(s.host!);
    } else if (s.onBle) {
      await discovery.stopBle();
      try {
        ble = await BleBox.connect(s.bleDevice!, s.name);
      } catch (_) {
        discovery.startBle();
        rethrow;
      }
      via = ble.midi;
      config = ble.config;
    } else {
      throw StateError('${s.name} is not on Wi-Fi or Bluetooth now');
    }
    var heard = 0;
    final sub = via.incoming.listen((m) {
      synth.device?.receive(m);
      if (m.isNotEmpty && const [0xB0, 0xD0, 0xE0].contains(m[0] & 0xF0)) heard++; // the report is these
    });
    try {
      if (ble != null) await Future.delayed(const Duration(milliseconds: 400)); // notifications on first
      await config.call('POST', '/api/ci/report');
      var r = 'no answer';
      for (var i = 0; i < 20; i++) {
        await Future.delayed(const Duration(milliseconds: 150));
        final st = await config.call('GET', '/api/ci');
        final x = st['report'] as String? ?? '';
        if (x != 'asking' && x != '') {
          r = x;
          break;
        }
      }
      await Future.delayed(const Duration(milliseconds: 300)); // the last values on their way
      if (heard > 0 || r == 'ok') keep(t.id, all: true);
      synth.changed();
      return (r, heard);
    } finally {
      await sub.cancel();
      if (ble != null) {
        await ble.close();
        discovery.startBle();
      } else {
        await via.close();
        await config.close();
      }
    }
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

// The desktop's MIDI router (docs/desktop.md "Direction"): routes from the computer's inputs to its outputs, run
// inside the engine (packages/midi_engine ame_route_set), so a routed message never waits for Dart. Channels keep
// things apart: a route passes a set of channels and can move each to another. Routes are kept by port name and
// saved, so they come back with the app and with a device that is plugged in again.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:midi_engine/midi_engine.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:airmidi/desktop/desktop_midi.dart';

class RouteSpec {
  String from, to; // an input's and an output's name (kVirtualName: the app's own ports)
  int mask; // bit c: channel c (0-based) passes
  final List<int> map; // channel c goes out on map[c]
  int transpose, lo, hi; // notes lo..hi pass, moved by transpose
  int types; // RouteTypes bits

  RouteSpec(this.from, this.to,
      {this.mask = 0xFFFF, List<int>? map, this.transpose = 0, this.lo = 0, this.hi = 127, int? types})
      : map = map ?? List.generate(16, (c) => c),
        // SysEx stays off by default: MIDI-CI between two boxes or apps can go round a loop
        types = types ?? RouteTypes.channel | RouteTypes.clock;

  bool passes(int ch) => mask & (1 << ch) != 0;

  /// "all", "1-4 10", "1→3": the channels that pass and where they go, for the grid's cells.
  String get summary {
    if (mask == 0) return 'none';
    final parts = <String>[];
    int? start, end;
    void flush() {
      if (start == null) return;
      parts.add(start == end ? '${start! + 1}' : '${start! + 1}-${end! + 1}');
      start = null;
    }

    for (var c = 0; c < 16; c++) {
      if (!passes(c)) {
        flush();
      } else if (map[c] != c) {
        flush();
        parts.add('${c + 1}→${map[c] + 1}');
      } else if (start != null && end == c - 1) {
        end = c;
      } else {
        flush();
        start = end = c;
      }
    }
    flush();
    final s = parts.join(' ');
    return [s == '1-16' ? 'all' : s, if (transpose != 0) '${transpose > 0 ? '+' : ''}$transpose'].join(' ');
  }

  Map<String, dynamic> toJson() =>
      {'from': from, 'to': to, 'mask': mask, 'map': map, 'transpose': transpose, 'lo': lo, 'hi': hi, 'types': types};

  static RouteSpec fromJson(Map<String, dynamic> j) => RouteSpec(j['from'] as String, j['to'] as String,
      mask: j['mask'] as int,
      map: [for (final c in j['map'] as List) c as int],
      transpose: j['transpose'] as int,
      lo: j['lo'] as int,
      hi: j['hi'] as int,
      types: j['types'] as int);
}

class MidiRouter extends ChangeNotifier {
  MidiRouter(this.engine) {
    _ports = engine.portsChanged.listen((_) => _apply());
    _load();
  }

  static MidiRouter? _instance;
  static MidiRouter get instance => _instance ??= MidiRouter(DesktopMidi.engine);

  final MidiEngine engine;
  final routes = <RouteSpec>[];
  late final StreamSubscription<void> _ports;
  final _in = <String, int>{}, _out = <String, int>{}; // the router's own handles, by port name
  final _ids = <RouteSpec, int>{};
  int _nextId = 1000; // the engine's route ids (the router is their only user)

  /// The inputs a route can come from (the app's own source excluded: it would route what the router sent).
  List<String> get sources => [
        kVirtualName,
        for (final p in engine.inputs)
          if (p.name != kVirtualName) p.name,
      ];

  /// The outputs a route can go to (the app's own destination excluded, for the same reason).
  List<String> get destinations => [
        kVirtualName,
        for (final p in engine.outputs)
          if (p.name != kVirtualName) p.name,
      ];

  RouteSpec? find(String from, String to) {
    for (final r in routes) {
      if (r.from == from && r.to == to) return r;
    }
    return null;
  }

  RouteSpec add(String from, String to) {
    var r = find(from, to);
    if (r == null) {
      r = RouteSpec(from, to);
      routes.add(r);
    }
    _apply();
    return r;
  }

  void remove(RouteSpec r) {
    _silence(r);
    routes.remove(r);
    _apply();
  }

  /// After editing [r]: notes still held through it end first (a note off sent another way would never arrive).
  void changed(RouteSpec r) {
    _silence(r);
    _apply();
  }

  /// Messages [r] passed so far, for the activity lights.
  int count(RouteSpec r) {
    final id = _ids[r];
    return id == null ? 0 : engine.routeCount(id).clamp(0, 1 << 62);
  }

  /// Whether the ports [r] needs are there now.
  bool live(RouteSpec r) => _ids.containsKey(r);

  void _silence(RouteSpec r) {
    final h = _out[r.to];
    if (h == null) return;
    for (var c = 0; c < 16; c++) {
      if (r.passes(c)) engine.send(h, [0xB0 | r.map[c], 123, 0]);
    }
  }

  int? _handle(String name, {required bool output}) {
    if (name == kVirtualName) {
      final (o, i) = DesktopMidi.virtualPorts();
      return output ? o : i;
    }
    final cache = output ? _out : _in;
    final h = cache[name];
    if (h != null) return h;
    final p = (output ? engine.outputs : engine.inputs).where((p) => p.name == name);
    if (p.isEmpty) return null;
    try {
      return cache[name] = engine.open(p.first);
    } catch (_) {
      return null;
    }
  }

  // Everything again from the list: ports may have come or gone (closing a port drops its routes in the engine).
  void _apply() {
    for (final id in _ids.values) {
      engine.removeRoute(id);
    }
    _ids.clear();
    for (final h in [..._in.values, ..._out.values]) {
      engine.close(h);
    }
    _in.clear();
    _out.clear();
    for (final r in routes) {
      final i = _handle(r.from, output: false), o = _handle(r.to, output: true);
      if (i == null || o == null) continue;
      final id = _nextId++;
      engine.setRoute(id, i, o, mask: r.mask, map: r.map, transpose: r.transpose, lo: r.lo, hi: r.hi, types: r.types);
      _ids[r] = id;
    }
    _save();
    notifyListeners();
  }

  static const _key = 'routes';

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final s = p.getString(_key);
    if (s == null) return;
    try {
      routes.addAll([for (final j in jsonDecode(s) as List) RouteSpec.fromJson(j as Map<String, dynamic>)]);
    } catch (_) {}
    _apply();
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_key, jsonEncode([for (final r in routes) r.toJson()]));
  }

  @override
  void dispose() {
    _ports.cancel();
    super.dispose();
  }
}

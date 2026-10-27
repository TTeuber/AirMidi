// Plays a [Song] through the desktop MIDI engine: every 20 ms the events of the next ~120 ms go to the engine with
// their times (sequencer.dart eventsBetween), and the engine's real-time thread sends each one when it is due.
// Armed clip tracks record from their input while playing (at the time the engine stamped each message) and pass
// what is played through to their output.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:midi_engine/midi_engine.dart';

import 'keys_bus.dart';
import 'sequencer.dart';

const _lookahead = 120000000; // ns scheduled ahead: covers a late timer tick, costs this much delay on a change
const _tick = Duration(milliseconds: 20);
const _startDelay = 60000000; // ns from pressing play to the first beat: room for the first window

class SeqPlayer extends ChangeNotifier {
  final Song song;
  final MidiEngine engine;
  SeqPlayer(this.song, this.engine) {
    _ports = engine.portsChanged.listen((_) => _reopen());
    _in = engine.events.listen((e) {
      final name = _inNames[e.handle];
      if (name != null) _input(name, e.bytes, e.tNs);
    });
    _keys = keysBus.stream.listen((e) => _input(kKeysInput, e.$1, e.$2));
  }

  final _inHandles = <String, int>{}, _inNames = <int, String>{};
  late final StreamSubscription<MidiInEvent> _in;
  late final StreamSubscription<(List<int>, int)> _keys;
  final _held = <ClipTrack, Set<(int, int)>>{}; // notes held while recording, ended at stop

  /// Opens the inputs the armed clip tracks listen to (call after arming or changing an input).
  void syncInputs() {
    final want = {
      for (final t in song.tracks.whereType<ClipTrack>())
        if (t.armed && t.input != null && t.input != kKeysInput) t.input!
    };
    for (final name in want) {
      if (_inHandles.containsKey(name)) continue;
      final p = engine.inputs.where((p) => p.name == name);
      if (p.isEmpty) continue;
      try {
        final h = engine.open(p.first);
        _inHandles[name] = h;
        _inNames[h] = name;
      } catch (e) {
        error = '$e';
      }
    }
    for (final name in _inHandles.keys.where((n) => !want.contains(n)).toList()) {
      final h = _inHandles.remove(name)!;
      _inNames.remove(h);
      engine.close(h);
    }
  }

  void _input(String input, List<int> bytes, int tNs) {
    if (bytes.isEmpty || bytes[0] >= 0xF0) return; // channel messages only: no clock, no SysEx
    final tl = _tl;
    for (final t in song.tracks.whereType<ClipTrack>()) {
      if (!t.armed || t.input != input) continue;
      if (t.port != null) _send(t.port!, bytes, 0); // hear what you play
      if (tl == null) continue;
      final beat = tl.beatAt(tNs);
      if (beat < 0) continue; // the count before the first beat
      t.record(beat, bytes);
      final type = bytes[0] & 0xF0, key = (bytes[0] & 0x0F, bytes.length > 1 ? bytes[1] : 0);
      final held = _held.putIfAbsent(t, () => {});
      if (type == 0x90 && bytes.length > 2 && bytes[2] > 0) held.add(key);
      if (type == 0x80 || (type == 0x90 && bytes.length > 2 && bytes[2] == 0)) held.remove(key);
      notifyListeners();
    }
  }

  Timeline? _tl;
  double _until = 0; // beats scheduled so far
  Timer? _timer;
  final _notes = <ScheduledNote>[];
  final _handles = <String, int>{};
  late final StreamSubscription<void> _ports;
  String? error;

  bool get playing => _tl != null;

  /// Where play is now, in beats (negative before the first beat; -1 when stopped).
  double get beat => _tl?.beatAt(engine.nowNs) ?? -1;

  int? _handle(String port) {
    final h = _handles[port];
    if (h != null) return h;
    final p = engine.outputs.where((p) => p.name == port);
    if (p.isEmpty) return null;
    try {
      return _handles[port] = engine.open(p.first);
    } catch (e) {
      error = '$e';
      return null;
    }
  }

  // A port that went away and came back is a new endpoint: open it again on next use.
  void _reopen() {
    if (playing) return;
    for (final h in _handles.values) {
      engine.close(h);
    }
    _handles.clear();
  }

  void _send(String port, List<int> bytes, int atNs) {
    final h = _handle(port);
    if (h == null) return;
    try {
      engine.send(h, bytes, atNs: atNs);
    } catch (e) {
      _handles.remove(port); // closed under us: reopened next time
    }
  }

  void play() {
    if (playing) return;
    error = null;
    final start = engine.nowNs + _startDelay;
    _tl = Timeline(start, song.bpm);
    _until = 0;
    for (final p in song.clockPorts) {
      _send(p, const [0xF2, 0, 0], start - 1000000); // Song Position 0, then Start on the first beat
      _send(p, const [0xFA], start);
    }
    _timer = Timer.periodic(_tick, (_) => _schedule());
    _schedule();
    notifyListeners();
  }

  void _schedule() {
    final tl = _tl;
    if (tl == null) return;
    final now = engine.nowNs;
    final to = tl.beatAt(now + _lookahead);
    if (to <= _until) return;
    for (final e in eventsBetween(song, tl, _until, to, notes: _notes)) {
      _send(e.port, e.bytes, e.tNs);
    }
    _until = to;
    _notes.removeWhere((n) => n.offNs < now);
  }

  /// Stops at once: what is queued is dropped, notes that may be sounding get their note off now.
  void stop() {
    if (!playing) return;
    _timer?.cancel();
    _timer = null;
    for (final h in _handles.values) {
      engine.clear(h);
    }
    final now = engine.nowNs;
    for (final n in _notes) {
      if (n.offNs >= now) _send(n.port, [0x80 | n.channel, n.note, 0], 0);
    }
    _notes.clear();
    // a recorded note still held: it ends where play stopped; clip playback: everything off, bends back to centre
    final stopBeat = _tl!.beatAt(now);
    for (final e in _held.entries) {
      for (final (ch, n) in e.value) {
        e.key.record(stopBeat, [0x80 | ch, n, 0]);
      }
    }
    _held.clear();
    for (final t in song.tracks.whereType<ClipTrack>()) {
      if (t.port == null) continue;
      for (final ch in t.channels) {
        _send(t.port!, [0xB0 | ch, 123, 0], 0);
        _send(t.port!, [0xE0 | ch, 0, 64], 0);
      }
    }
    for (final p in song.clockPorts) {
      _send(p, const [0xFC], 0);
    }
    _tl = null;
    notifyListeners();
  }

  /// A new tempo. While playing it takes over where scheduling has got to (up to ~120 ms ahead already went out at
  /// the old one), so the beats on either side of the change meet without a gap.
  void setBpm(double bpm) {
    song.bpm = bpm.clamp(20, 300);
    final tl = _tl;
    if (tl != null) {
      final at = tl.nsAt(_until);
      tl
        ..anchorBeat = _until
        ..anchorNs = at
        ..bpm = song.bpm;
    }
    notifyListeners();
  }

  @override
  void dispose() {
    stop();
    _ports.cancel();
    _in.cancel();
    _keys.cancel();
    for (final h in _handles.values) {
      engine.close(h);
    }
    super.dispose();
  }
}

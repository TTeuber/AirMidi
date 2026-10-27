// Plays a [Song] through the desktop MIDI engine: every 20 ms the events of the next ~120 ms go to the engine with
// their times (sequencer.dart eventsBetween), and the engine's real-time thread sends each one when it is due.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:midi_engine/midi_engine.dart';

import 'sequencer.dart';

const _lookahead = 120000000; // ns scheduled ahead: covers a late timer tick, costs this much delay on a change
const _tick = Duration(milliseconds: 20);
const _startDelay = 60000000; // ns from pressing play to the first beat: room for the first window

class SeqPlayer extends ChangeNotifier {
  final Song song;
  final MidiEngine engine;
  SeqPlayer(this.song, this.engine) {
    _ports = engine.portsChanged.listen((_) => _reopen());
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
    for (final h in _handles.values) {
      engine.close(h);
    }
    super.dispose();
  }
}

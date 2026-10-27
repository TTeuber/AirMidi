// Plays a [Project] through the desktop MIDI engine (docs/daw.md "Playback"): every 20 ms the events of the next
// ~120 ms go to the engine with their times (compile.dart), and the engine's real-time thread sends each one when it
// is due. Play runs on a continuous beat count from the start position; with the loop on, that count maps back into
// the loop range, so a stretch that crosses the loop's end is compiled in two pieces and notes end at the loop end.
// MIDI clock follows the continuous count, so a drum machine keeps steady time across the loop.
//
// Recording: armed tracks pass what their input plays through to their device (monitor), and while recording each
// gets a take (record.dart) that the player rebuilds into its clip every tick.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:midi_engine/midi_engine.dart';

import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/keys_bus.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/record.dart';
import 'package:airmidi/desktop/daw/time.dart';
import 'package:airmidi/desktop/desktop_midi.dart';

const _lookahead = 120000000; // ns scheduled ahead: covers a late timer tick, costs this much delay on a change
const _tick = Duration(milliseconds: 20);
const _startDelay = 60000000; // ns from pressing play to the first beat: room for the first window

class DawPlayer extends ChangeNotifier {
  final Project project;
  final History history;
  final MidiEngine engine;
  DawPlayer(this.project, this.history, this.engine) {
    _ports = engine.portsChanged.listen((_) => _reopen());
    _in = engine.events.listen((e) {
      final name = e.handle == DesktopMidi.virtualPorts().$2 ? kVirtualName : _inNames[e.handle];
      if (name != null) _input(name, e.bytes, e.tNs);
    });
    _keys = keysBus.stream.listen((e) => _input(kKeysInput, e.$1, e.$2));
  }

  /// Where a track can send: the app's own port (to Logic and other apps) and the computer's outputs.
  List<String> get outputs => [kVirtualName, for (final p in DesktopMidi.outputs()) p.name];

  /// What a track can record: the keys page, the app's own port (what Logic sends it), the computer's inputs.
  List<String> get inputs => [
        kKeysInput,
        kVirtualName,
        for (final p in engine.inputs)
          if (p.name != kVirtualName) p.name,
      ];

  final _inHandles = <String, int>{}, _inNames = <int, String>{};
  late final StreamSubscription<MidiInEvent> _in;
  late final StreamSubscription<(List<int>, int)> _keys;
  late final StreamSubscription<void> _ports;
  final _handles = <String, int>{};
  final _notes = <ScheduledNote>[];
  final takes = <Track, Take>{};
  Timeline? _tl;
  double _until = 0; // continuous beats scheduled so far
  Timer? _timer;
  bool recording = false;
  bool replace = false; // recording in the loop range: the first pass clears the clip instead of adding to it
  bool chaseOn = true; // controllers where the song has them when play starts and when the loop starts again
  String? error;

  bool get playing => _tl != null;

  /// Opens the inputs the armed tracks listen to (call after arming or changing an input).
  void syncInputs() {
    final want = {
      for (final t in project.tracks)
        if (t.armed && t.input != null && t.input != kKeysInput && t.input != kVirtualName) t.input!
    };
    if (project.tracks.any((t) => t.armed && t.input == kVirtualName)) DesktopMidi.virtualPorts();
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

  bool get _looping => project.loopOn && project.loopEnd > project.loopStart && project.startBeat < project.loopEnd;

  /// The song position of continuous beat [c], and which pass through the loop it is in.
  (double, int) songAt(double c) {
    if (!_looping || c < project.loopEnd) return (c, 0);
    final len = project.loopEnd - project.loopStart, k = ((c - project.loopEnd) / len).floor();
    return (project.loopStart + (c - project.loopEnd) - k * len, k + 1);
  }

  /// Where play is now, in song beats (the start position when stopped).
  double get beat {
    final tl = _tl;
    if (tl == null) return project.startBeat;
    final c = tl.beatAt(engine.nowNs);
    return c < project.startBeat ? project.startBeat : songAt(c).$1;
  }

  void _input(String input, List<int> bytes, int tNs) {
    if (bytes.isEmpty || bytes[0] >= 0xF0) return; // channel messages only: no clock, no SysEx
    final tl = _tl;
    for (final t in project.tracks) {
      if (!t.armed || t.input != input) continue;
      if (t.monitor && t.port != null) {
        // hear what you play on the track's device; a track that is not MPE plays on its own channel
        _send(t.port!, t.mpe ? bytes : [(bytes[0] & 0xF0) | t.channel, ...bytes.skip(1)], 0);
      }
      final take = takes[t];
      if (take == null || tl == null) continue;
      final c = tl.beatAt(tNs);
      if (c < project.startBeat) continue; // before the first beat
      take.add(songAt(c).$1, bytes);
    }
  }

  int? _handle(String port) {
    if (port == kVirtualName) return DesktopMidi.virtualPorts().$1;
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

  void play({bool record = false}) {
    if (playing) return;
    error = null;
    final start = engine.nowNs + _startDelay, from = project.startBeat;
    _tl = Timeline(start, project.bpm, anchorBeat: from);
    _until = from;
    final spp = (from * 4).round().clamp(0, 16383); // Song Position in sixteenths
    for (final p in project.clockPorts) {
      _send(p, [0xF2, spp & 0x7F, spp >> 7], start - 1000000);
      _send(p, [spp == 0 ? 0xFA : 0xFB], start); // Start, or Continue from the position
    }
    if (chaseOn) {
      for (final e in chase(project, from, start - 500000)) {
        _send(e.port, e.bytes, e.tNs); // half a millisecond before the first beat
      }
    }
    if (record) setRecording(true);
    _timer = Timer.periodic(_tick, (_) => _schedule());
    _schedule();
    notifyListeners();
  }

  /// Recording on or off; on while playing punches in from where play is.
  void setRecording(bool on) {
    if (on == recording) return;
    recording = on;
    if (on && playing) _startTakes(beat);
    if (!on) _finishTakes(beat);
    notifyListeners();
  }

  void _startTakes(double at) {
    final armed = project.tracks.where((t) => t.armed).toList();
    if (armed.isEmpty) return;
    history.mark(); // undo takes the whole take away
    for (final t in armed) {
      takes[t] = Take.start(project, t, at, replace: replace);
    }
  }

  void _finishTakes(double at) {
    for (final take in takes.values) {
      take.finish(at);
    }
    takes.clear();
  }

  void _schedule() {
    final tl = _tl;
    if (tl == null) return;
    final now = engine.nowNs;
    if (takes.isNotEmpty) {
      final c = tl.beatAt(now);
      for (final take in takes.values) {
        take.flush(c < project.startBeat ? project.startBeat : songAt(c).$1);
      }
      notifyListeners();
    }
    final to = tl.beatAt(now + _lookahead);
    if (to <= _until) return;
    final out = <SeqEvent>[];
    // the window in pieces that each lie inside one pass of the loop; notes end at the loop's end
    var a = _until;
    while (a < to) {
      final (s, pass) = songAt(a);
      final delta = a - s;
      var end = _looping ? math.min(to, a + (project.loopEnd - s)) : to;
      if (end <= a) end = to; // rounding at the loop's end
      if (chaseOn && pass > 0 && (s - project.loopStart).abs() < 1e-9) {
        // the loop starts again: controllers back to where they are at its start
        out.addAll(chase(project, project.loopStart, tl.nsAt(s + delta) - 1000));
      }
      out.addAll(compile(project, s, end - delta, (b) => tl.nsAt(b + delta),
          cut: _looping ? project.loopEnd : double.infinity, pass: pass, notes: _notes));
      a = end;
    }
    if (project.clockPorts.isNotEmpty) {
      for (var j = ((_until - project.startBeat) * 24).ceil(); project.startBeat + j / 24 < to; j++) {
        final at = tl.nsAt(project.startBeat + j / 24);
        for (final p in project.clockPorts) {
          out.add(SeqEvent(at, p, const [0xF8]));
        }
      }
    }
    sortEvents(out);
    for (final e in out) {
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
    _finishTakes(beat);
    recording = false;
    for (final h in _handles.values) {
      engine.clear(h);
    }
    final now = engine.nowNs;
    for (final n in _notes) {
      if (n.offNs >= now) _send(n.port, [0x80 | n.channel, n.note, 0], 0);
    }
    _notes.clear();
    // MIDI clips may have bent or held things: everything off, bends back to centre
    for (final t in project.tracks) {
      if (t.port == null || !t.clips.any((c) => c.content is MidiContent)) continue;
      for (final ch in t.mpe ? List.generate(16, (c) => c) : [t.channel]) {
        _send(t.port!, [0xB0 | ch, 123, 0], 0);
        _send(t.port!, [0xE0 | ch, 0, 64], 0);
      }
    }
    for (final p in project.clockPorts) {
      _send(p, const [0xFC], 0);
    }
    _tl = null;
    notifyListeners();
  }

  /// [bytes] to [port] now (the device tab's controls).
  void sendNow(String port, List<int> bytes) => _send(port, bytes, 0);

  /// A note on or off on [t]'s device now (the piano roll's keys, a note drawn or moved).
  void audition(Track t, int pitch, bool on, int velocity) {
    final port = t.port;
    if (port == null) return;
    final ch = t.mpe ? 1 : t.channel;
    _send(port, [(on ? 0x90 : 0x80) | ch, pitch, on ? velocity.clamp(1, 127) : 0], 0);
  }

  /// A new tempo. While playing it takes over where scheduling has got to (up to ~120 ms ahead already went out at
  /// the old one), so the beats on either side of the change meet without a gap.
  void setBpm(double bpm) {
    project.bpm = bpm.clamp(20, 300);
    final tl = _tl;
    if (tl != null) {
      final at = tl.nsAt(_until);
      tl
        ..anchorBeat = _until
        ..anchorNs = at
        ..bpm = project.bpm;
    }
    notifyListeners();
  }

  @override
  void dispose() {
    stop();
    _ports.cancel();
    _in.cancel();
    _keys.cancel();
    for (final h in [..._handles.values, ..._inHandles.values]) {
      engine.close(h);
    }
    super.dispose();
  }
}

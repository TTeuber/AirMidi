// The tracks page's sequencer (docs/desktop.md D3): step tracks on one transport, and MIDI clock out. Pure logic:
// a song and a time window in, timestamped MIDI events out. The scheduler (tracks/player.dart) asks for the next
// ~120 ms every 20 ms and hands the events to the MIDI engine with their times, so Dart's timer jitter never
// reaches the notes.
//
// Time: beats (quarter notes) from the start of play, mapped to the engine's clock (ns) through a [Timeline] that
// is re-anchored on every tempo change, so the position never jumps.

/// A step track's row: which note it plays and what to call it.
class StepRow {
  final int note;
  final String label;
  const StepRow(this.note, this.label);
}

class StepTrack {
  String name;
  String? port; // an output's name; null = not sending yet
  int channel; // 0-based
  final List<StepRow> rows;
  int steps; // the track's length: tracks of different lengths drift against each other (polymeter)
  bool mute = false;
  double gate; // a note's length, in steps
  final List<List<int>> cells; // [row][step]: velocity, 0 = off

  StepTrack({
    required this.name,
    required this.rows,
    this.port,
    this.channel = 9,
    this.steps = 16,
    this.gate = 0.5,
  }) : cells = [for (final _ in rows) List.filled(64, 0)]; // room for up to 64 steps

  bool on(int row, int step) => cells[row][step] > 0;

  void toggle(int row, int step, {int velocity = 100}) => cells[row][step] = on(row, step) ? 0 : velocity;
}

/// A drum track on the General MIDI map: the rows a beat mostly uses, kick at the bottom.
StepTrack drumTrack({String? port}) => StepTrack(name: 'drums', port: port, channel: 9, rows: const [
      StepRow(49, 'crash'),
      StepRow(46, 'open hat'),
      StepRow(42, 'hat'),
      StepRow(39, 'clap'),
      StepRow(38, 'snare'),
      StepRow(36, 'kick'),
    ]);

const _names = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b'];

/// A note track: an octave of pitches from [low] (top row highest), for a synth's bass line or arpeggio.
StepTrack noteTrack({String? port, int low = 48, int channel = 0}) => StepTrack(
      name: 'notes',
      port: port,
      channel: channel,
      gate: 0.8,
      rows: [for (var n = low + 12; n >= low; n--) StepRow(n, '${_names[n % 12]}${n ~/ 12 - 1}')],
    );

class Song {
  double bpm;
  int stepsPerBeat; // 4: sixteenths
  final List<StepTrack> tracks;
  final Set<String> clockPorts = {}; // outputs that get MIDI clock, Start and Stop
  Song({this.bpm = 120, this.stepsPerBeat = 4, List<StepTrack>? tracks}) : tracks = tracks ?? [];
}

/// Beats <-> engine time at the current tempo, from an anchor (where the last tempo change happened).
class Timeline {
  int anchorNs;
  double anchorBeat;
  double bpm;
  Timeline(this.anchorNs, this.bpm, {this.anchorBeat = 0});

  double beatAt(int ns) => anchorBeat + (ns - anchorNs) * bpm / 60e9;
  int nsAt(double beat) => anchorNs + ((beat - anchorBeat) * 60e9 / bpm).round();

  /// A new tempo from [nowNs] on; the beats before it keep their times.
  void setBpm(double b, int nowNs) {
    anchorBeat = beatAt(nowNs);
    anchorNs = nowNs;
    bpm = b;
  }
}

class SeqEvent {
  final int tNs;
  final String port;
  final List<int> bytes;
  const SeqEvent(this.tNs, this.port, this.bytes);

  @override
  String toString() => '$tNs $port $bytes';
}

/// A note that was scheduled: what has to be ended if play stops before its note off goes out.
class ScheduledNote {
  final String port;
  final int channel, note, offNs;
  const ScheduledNote(this.port, this.channel, this.note, this.offNs);
}

/// Everything due in [from, to) beats: each track's notes (on and off) and the clock ticks (24 per beat).
/// [notes] collects the notes started, for [Song] stop handling.
List<SeqEvent> eventsBetween(Song song, Timeline tl, double from, double to, {List<ScheduledNote>? notes}) {
  final out = <SeqEvent>[];
  final spb = song.stepsPerBeat;
  for (var k = (from * spb).ceil(); k / spb < to; k++) {
    final at = tl.nsAt(k / spb);
    for (final t in song.tracks) {
      if (t.mute || t.port == null || t.steps <= 0) continue;
      final step = k % t.steps;
      for (var r = 0; r < t.rows.length; r++) {
        final v = t.cells[r][step];
        if (v <= 0) continue;
        final n = t.rows[r].note, off = tl.nsAt((k + t.gate) / spb);
        out.add(SeqEvent(at, t.port!, [0x90 | t.channel, n, v.clamp(1, 127)]));
        out.add(SeqEvent(off, t.port!, [0x80 | t.channel, n, 0]));
        notes?.add(ScheduledNote(t.port!, t.channel, n, off));
      }
    }
  }
  if (song.clockPorts.isNotEmpty) {
    for (var j = (from * 24).ceil(); j / 24 < to; j++) {
      final at = tl.nsAt(j / 24);
      for (final p in song.clockPorts) {
        out.add(SeqEvent(at, p, const [0xF8]));
      }
    }
  }
  // by time; at the same time note offs first (a gate of a whole step ends a note just as the next one starts), then
  // the order they were made in (Dart's sort is not stable)
  final order = {for (var i = 0; i < out.length; i++) out[i]: i};
  int rank(SeqEvent e) => (e.bytes[0] & 0xF0) == 0x90 ? 1 : 0;
  out.sort((a, b) => a.tNs != b.tNs
      ? a.tNs.compareTo(b.tNs)
      : rank(a) != rank(b)
          ? rank(a).compareTo(rank(b))
          : order[a]!.compareTo(order[b]!));
  return out;
}

/// The step a track shows as playing at [beat].
int stepAt(StepTrack t, int stepsPerBeat, double beat) =>
    beat < 0 ? -1 : (beat * stepsPerBeat).floor() % (t.steps <= 0 ? 1 : t.steps);

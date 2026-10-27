// The tracks page's sequencer (docs/desktop.md D3, D4): step tracks and clip tracks on one transport, and MIDI
// clock out. Pure logic:
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

/// What every track has: a name, the output it plays to (null = not sending yet), mute.
abstract class Track {
  String name;
  String? port;
  bool mute = false;
  Track(this.name, this.port);
}

class StepTrack extends Track {
  int channel; // 0-based
  final List<StepRow> rows;
  int steps; // the track's length: tracks of different lengths drift against each other (polymeter)
  double gate; // a note's length, in steps
  final List<List<int>> cells; // [row][step]: velocity, 0 = off

  StepTrack({
    required String name,
    required this.rows,
    String? port,
    this.channel = 9,
    this.steps = 16,
    this.gate = 0.5,
  })  : cells = [for (final _ in rows) List.filled(64, 0)], // room for up to 64 steps
        super(name, port);

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

/// A recorded message, at its position in the clip (beats from the clip's start).
class ClipEvent {
  final double beat;
  final List<int> bytes;
  const ClipEvent(this.beat, this.bytes);
}

/// Recorded MIDI that loops: whatever arrived from [input] while armed and playing, as it came (MPE stays MPE:
/// every note on its own channel with its own bends). Plays back on the channels it was recorded on.
class ClipTrack extends Track {
  String? input; // an input's name, or kKeysInput for the app's own keys page
  bool armed = false; // records while playing, and passes what is played through to [port]
  double lengthBeats;
  final List<ClipEvent> events = []; // kept sorted by beat

  ClipTrack({String name = 'clip', String? port, this.input, this.lengthBeats = 16}) : super(name, port);

  /// Adds a recorded message at [beat] of the song (wrapped into the clip).
  void record(double beat, List<int> bytes) {
    final at = beat % lengthBeats;
    var i = events.length;
    while (i > 0 && events[i - 1].beat > at) {
      i--;
    }
    events.insert(i, ClipEvent(at, bytes));
  }

  Set<int> get channels => {
        for (final e in events)
          if (e.bytes[0] < 0xF0) e.bytes[0] & 0x0F
      };

  /// Notes as bars, for drawing: (channel, note, start, end) in clip beats; a note still held at the end runs to it.
  List<(int, int, double, double)> notes() {
    final open = <(int, int), double>{};
    final out = <(int, int, double, double)>[];
    for (final e in events) {
      final b = e.bytes, t = b[0] & 0xF0, k = (b[0] & 0x0F, b.length > 1 ? b[1] : 0);
      if (t == 0x90 && b[2] > 0) {
        open[k] = e.beat;
      } else if (t == 0x80 || (t == 0x90 && b[2] == 0)) {
        final s = open.remove(k);
        if (s != null) out.add((k.$1, k.$2, s, e.beat));
      }
    }
    for (final e in open.entries) {
      out.add((e.key.$1, e.key.$2, e.value, lengthBeats));
    }
    return out;
  }
}

/// The input name that means "the keys page of this app".
const kKeysInput = 'keys page';

class Song {
  double bpm;
  int stepsPerBeat; // 4: sixteenths
  final List<Track> tracks;
  final Set<String> clockPorts = {}; // outputs that get MIDI clock, Start and Stop
  Song({this.bpm = 120, this.stepsPerBeat = 4, List<Track>? tracks}) : tracks = tracks ?? [];
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
  for (final t in song.tracks.whereType<ClipTrack>()) {
    if (t.mute || t.port == null || t.lengthBeats <= 0 || t.events.isEmpty) continue;
    final len = t.lengthBeats;
    for (var loop = (from / len).floor(); loop * len < to; loop++) {
      for (final e in t.events) {
        final b = loop * len + e.beat;
        if (b >= from && b < to) out.add(SeqEvent(tl.nsAt(b), t.port!, e.bytes));
      }
    }
  }
  for (var k = (from * spb).ceil(); k / spb < to; k++) {
    final at = tl.nsAt(k / spb);
    for (final t in song.tracks.whereType<StepTrack>()) {
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

// The DAW's project (docs/daw.md "Model"): tracks that each send to one device, holding clips on a timeline. A clip
// is MIDI (notes in free time, controller lanes) or a pattern (rows by steps); every clip owns its content, and a
// copy copies it. Pure Dart: the arrangement page edits it, compile.dart turns it into timed MIDI, record.dart fills
// MIDI clips from what is played.
//
// Times are beats (quarter notes) as doubles. Values are kept at MIDI 2.0 resolution (velocity 16 bit, controllers
// 32 bit) and scaled down for MIDI 1.0 ports, so the model does not change when MIDI 2.0 ports arrive.

int _lastId = 0;
int newId() => ++_lastId;

// MIDI 1.0 <-> MIDI 2.0 values (the min-center-max scaling of M2-115: 0 stays 0, the centre stays the centre, the
// top reaches the top; scaling down is a shift, so a 7-bit value comes back exactly).
int _up(int v, int from, int to) {
  final scale = to - from, centre = 1 << (from - 1);
  var out = v << scale;
  if (v <= centre) return out;
  final repBits = from - 1;
  var rep = v & ((1 << repBits) - 1);
  rep = scale > repBits ? rep << (scale - repBits) : rep >> (repBits - scale);
  while (rep != 0) {
    out |= rep;
    rep >>= repBits;
  }
  return out;
}

int up7to16(int v) => _up(v.clamp(0, 127), 7, 16);
int up7to32(int v) => _up(v.clamp(0, 127), 7, 32);
int up14to32(int v) => _up(v.clamp(0, 16383), 14, 32);
int down16to7(int v) => v >> 9;
int down32to7(int v) => v >> 25;
int down32to14(int v) => v >> 18;

const kCentre32 = 0x80000000; // a bend's centre at 32 bit

/// A value at a time: a controller lane's point (beats from the content's start) or a note's expression point
/// (beats from the note's start).
class Point {
  final double t;
  final int v;
  const Point(this.t, this.v);
}

class Note {
  double start, length; // beats from the content's start
  int pitch;
  int velocity, release; // 16 bit
  /// MPE: the note's own 'bend', 'pressure' and 'timbre' (CC74), points from the note's start, 32 bit.
  final Map<String, List<Point>> expression;

  Note(this.start, this.length, this.pitch, {int? velocity, this.release = 0, Map<String, List<Point>>? expression})
      : velocity = velocity ?? up7to16(100),
        expression = expression ?? {};

  double get end => start + length;

  Note copy() => Note(start, length, pitch, velocity: velocity, release: release, expression: {
        for (final e in expression.entries) e.key: [...e.value]
      });
}

/// A message that is neither a note nor a lane's controller (SysEx, ...), kept as it came.
class RawEvent {
  final double t;
  final List<int> bytes;
  const RawEvent(this.t, this.bytes);
}

abstract class Content {
  double get length; // beats; a clip longer than this repeats it
  Content copy();
}

/// Lanes by name: `cc<n>` (n 0..127), 'bend', 'pressure' (channel pressure), 'program'.
class MidiContent extends Content {
  @override
  double length;
  final List<Note> notes;
  final Map<String, List<Point>> lanes;
  final List<RawEvent> other;

  MidiContent(this.length, {List<Note>? notes, Map<String, List<Point>>? lanes, List<RawEvent>? other})
      : notes = notes ?? [],
        lanes = lanes ?? {},
        other = other ?? [];

  @override
  MidiContent copy() => MidiContent(length, notes: [
        for (final n in notes) n.copy()
      ], lanes: {
        for (final l in lanes.entries) l.key: [...l.value]
      }, other: [
        ...other
      ]);
}

class Step {
  int velocity; // 16 bit
  double gate; // in steps
  double probability; // 0..1
  double nudge; // in steps, -0.5..0.5
  int repeat; // hits in the step, 1..4
  Step({int? velocity, this.gate = 0.5, this.probability = 1, this.nudge = 0, this.repeat = 1})
      : velocity = velocity ?? up7to16(100);
  Step copy() => Step(velocity: velocity, gate: gate, probability: probability, nudge: nudge, repeat: repeat);
}

class PatternRow {
  int note;
  String name;
  bool mute;
  PatternRow(this.note, this.name, {this.mute = false});
  PatternRow copy() => PatternRow(note, name, mute: mute);
}

const kMaxSteps = 64;

class Pattern extends Content {
  final List<PatternRow> rows;
  int steps;
  double stepBeats; // 0.25: sixteenths
  double swing; // 0.5 = straight .. 0.75: where the second step of each pair falls
  final List<List<Step?>> cells; // [row][step], kMaxSteps wide

  Pattern(this.rows, {this.steps = 16, this.stepBeats = 0.25, this.swing = 0.5, List<List<Step?>>? cells})
      : cells = cells ?? [for (final _ in rows) List<Step?>.filled(kMaxSteps, null)];

  @override
  double get length => steps * stepBeats;

  void toggle(int row, int step, {int? velocity}) =>
      cells[row][step] = cells[row][step] == null ? Step(velocity: velocity) : null;

  void addRow(PatternRow r) {
    rows.add(r);
    cells.add(List<Step?>.filled(kMaxSteps, null));
  }

  void removeRow(int i) {
    rows.removeAt(i);
    cells.removeAt(i);
  }

  @override
  Pattern copy() => Pattern([for (final r in rows) r.copy()],
      steps: steps,
      stepBeats: stepBeats,
      swing: swing,
      cells: [
        for (final row in cells) [for (final s in row) s?.copy()]
      ]);
}

/// The drum rows a beat mostly uses, General MIDI, kick at the bottom.
List<PatternRow> gmDrumRows() => [
      PatternRow(49, 'crash'),
      PatternRow(46, 'open hat'),
      PatternRow(42, 'hat'),
      PatternRow(39, 'clap'),
      PatternRow(38, 'snare'),
      PatternRow(36, 'kick'),
    ];

const _names = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b'];
String pitchName(int n) => '${_names[n % 12]}${n ~/ 12 - 1}';

/// An octave of pitches from [low] up (the highest row on top), for a bass line or an arpeggio.
List<PatternRow> chromaticRows(int low) => [for (var n = low + 12; n >= low; n--) PatternRow(n, pitchName(n))];

/// A piece of the timeline: [length] beats from [start], playing its content from [offset] on (and repeating it
/// when the clip is longer, if [loops]).
class Clip {
  final int id;
  String name;
  double start, length, offset;
  bool loops;
  Content content;

  Clip(this.name, this.start, this.length, this.content, {this.offset = 0, this.loops = true, int? id})
      : id = id ?? newId();

  double get end => start + length;
  bool get isPattern => content is Pattern;

  /// A copy with a new id (for duplicate and paste); [keepId] for undo snapshots.
  Clip copy({bool keepId = false}) =>
      Clip(name, start, length, content.copy(), offset: offset, loops: loops, id: keepId ? id : null);
}

class Track {
  final int id;
  String name;
  String? port; // the output it plays to (null: not sending yet)
  int channel; // 0-based; on an MPE track the zone's manager channel is channel 1 and the members 2..16
  bool mpe; // notes go out on member channels handed out afresh, each with its own expression
  String? input; // what it records (an input's name, or kKeysInput)
  bool armed = false, monitor = true, mute = false, solo = false;
  final List<Clip> clips;

  Track(this.name, {this.port, this.channel = 0, this.mpe = false, this.input, List<Clip>? clips, int? id})
      : clips = clips ?? [],
        id = id ?? newId();

  Track copy({bool keepId = false}) {
    final t = Track(name,
        port: port,
        channel: channel,
        mpe: mpe,
        input: input,
        clips: [for (final c in clips) c.copy(keepId: keepId)],
        id: keepId ? id : null);
    t
      ..armed = armed
      ..monitor = monitor
      ..mute = mute
      ..solo = solo;
    return t;
  }
}

/// The input name that means "the keys page of this app".
const kKeysInput = 'keys page';

class Project {
  double bpm;
  int beatsPerBar;
  bool loopOn;
  double loopStart, loopEnd;
  double startBeat; // where play starts
  final List<Track> tracks;
  final Set<String> clockPorts; // outputs that get MIDI clock, Start and Stop

  Project(
      {this.bpm = 120,
      this.beatsPerBar = 4,
      this.loopOn = false,
      this.loopStart = 0,
      this.loopEnd = 16,
      this.startBeat = 0,
      List<Track>? tracks,
      Set<String>? clockPorts})
      : tracks = tracks ?? [],
        clockPorts = clockPorts ?? {};

  /// Tracks that sound: not muted, and soloed if any track is.
  Iterable<Track> get audible {
    final solo = tracks.any((t) => t.solo);
    return tracks.where((t) => !t.mute && (!solo || t.solo));
  }

  Track? trackOf(Clip c) {
    for (final t in tracks) {
      if (t.clips.contains(c)) return t;
    }
    return null;
  }

  Project snapshot() => Project(
      bpm: bpm,
      beatsPerBar: beatsPerBar,
      loopOn: loopOn,
      loopStart: loopStart,
      loopEnd: loopEnd,
      startBeat: startBeat,
      tracks: [for (final t in tracks) t.copy(keepId: true)],
      clockPorts: {...clockPorts});

  /// Becomes [s] (undo): this object stays the one everybody holds.
  void restore(Project s) {
    bpm = s.bpm;
    beatsPerBar = s.beatsPerBar;
    loopOn = s.loopOn;
    loopStart = s.loopStart;
    loopEnd = s.loopEnd;
    startBeat = s.startBeat;
    tracks
      ..clear()
      ..addAll([for (final t in s.tracks) t.copy(keepId: true)]);
    clockPorts
      ..clear()
      ..addAll(s.clockPorts);
  }
}

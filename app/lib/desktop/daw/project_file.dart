// A project as a file (docs/daw.md "Projects and files"): readable JSON with a format name and version, one file per
// project (.airmidi). Everything the project is; nothing about the view (zoom, scroll) or the moment (armed tracks).
// Values are written as they are kept (velocity 16 bit, controllers 32 bit); steps and step settings only where they
// differ from the defaults, so a pattern stays short to read. Clip ids are kept: a pattern's probabilities roll from
// its clip's id, so a reopened song makes the same choices.
import 'dart:convert';

import 'package:airmidi/desktop/daw/model.dart';

const kProjectFormat = 'airmidi project', kProjectVersion = 1, kProjectExt = 'airmidi';

String encodeProject(Project p) => const JsonEncoder.withIndent(' ').convert(projectToJson(p));

Project decodeProject(String text) => projectFromJson(jsonDecode(text) as Map<String, dynamic>);

Map<String, dynamic> projectToJson(Project p) => {
      'format': kProjectFormat,
      'version': kProjectVersion,
      'bpm': p.bpm,
      'beatsPerBar': p.beatsPerBar,
      'loop': {'on': p.loopOn, 'start': p.loopStart, 'end': p.loopEnd},
      'start': p.startBeat,
      'clockTo': [...p.clockPorts],
      'click': {
        if (p.click.port != null) 'port': p.click.port,
        'channel': p.click.channel,
        'note': p.click.note,
        if (p.click.on) 'on': true,
      },
      'countIn': p.countIn,
      'tracks': [for (final t in p.tracks) _track(t)],
    };

Map<String, dynamic> _track(Track t) => {
      'id': t.id,
      'name': t.name,
      if (t.port != null) 'port': t.port,
      'channel': t.channel,
      if (t.mpe) 'mpe': true,
      if (t.input != null) 'input': t.input,
      if (!t.monitor) 'monitor': false,
      if (t.mute) 'mute': true,
      if (t.solo) 'solo': true,
      if (t.device != null) 'device': t.device,
      if (t.box != null) 'box': t.box,
      if (t.program != null) 'program': t.program,
      if (t.bankMsb != null || t.bankLsb != null) 'bank': [t.bankMsb, t.bankLsb],
      if (t.state.isNotEmpty) 'state': t.state,
      'clips': [for (final c in t.clips) _clip(c)],
    };

Map<String, dynamic> _clip(Clip c) {
  final content = c.content;
  return {
    'id': c.id,
    'name': c.name,
    'start': c.start,
    'length': c.length,
    if (c.offset != 0) 'offset': c.offset,
    if (!c.loops) 'loops': false,
    if (content is MidiContent) 'midi': _midi(content),
    if (content is Pattern) 'pattern': _pattern(content),
  };
}

List<List<num>> _points(List<Point> ps) => [
      for (final p in ps) [p.t, p.v, if (p.ramp) 1]
    ];

Map<String, dynamic> _midi(MidiContent m) => {
      'length': m.length,
      'notes': [
        for (final n in m.notes)
          {
            't': n.start,
            'len': n.length,
            'pitch': n.pitch,
            'vel': n.velocity,
            if (n.release != 0) 'rel': n.release,
            if (n.expression.isNotEmpty) 'expr': {for (final e in n.expression.entries) e.key: _points(e.value)},
          }
      ],
      if (m.lanes.isNotEmpty) 'lanes': {for (final l in m.lanes.entries) l.key: _points(l.value)},
      if (m.other.isNotEmpty)
        'other': [
          for (final r in m.other) [r.t, r.bytes]
        ],
    };

final _defaultStep = Step();

Map<String, dynamic> _pattern(Pattern p) => {
      'steps': p.steps,
      'stepBeats': p.stepBeats,
      if (p.swing != 0.5) 'swing': p.swing,
      'rows': [
        for (var r = 0; r < p.rows.length; r++)
          {
            'note': p.rows[r].note,
            'name': p.rows[r].name,
            if (p.rows[r].mute) 'mute': true,
            'cells': [
              for (var s = 0; s < kMaxSteps; s++)
                if (p.cells[r][s] case final st?)
                  {
                    'i': s,
                    if (st.velocity != _defaultStep.velocity) 'vel': st.velocity,
                    if (st.gate != _defaultStep.gate) 'gate': st.gate,
                    if (st.probability != 1) 'prob': st.probability,
                    if (st.nudge != 0) 'nudge': st.nudge,
                    if (st.repeat != 1) 'repeat': st.repeat,
                    if (st.locks.isNotEmpty) 'locks': st.locks,
                  }
            ],
          }
      ],
    };

double _d(Object? v, [double or = 0]) => v == null ? or : (v as num).toDouble();

List<Point> _readPoints(Object? v) => [
      for (final p in (v as List? ?? const []))
        if (p case final List p) Point(_d(p[0]), (p[1] as num).toInt(), ramp: p.length > 2 && p[2] == 1)
    ];

/// Reads a project; throws FormatException for a file that is not one (or is newer than this app).
Project projectFromJson(Map<String, dynamic> j) {
  if (j['format'] != kProjectFormat) throw const FormatException('not an airmidi project');
  final version = (j['version'] as num?)?.toInt() ?? 0;
  if (version > kProjectVersion) throw FormatException('made by a newer airmidi (format $version)');
  final loop = j['loop'] as Map<String, dynamic>? ?? const {};
  return Project(
    bpm: _d(j['bpm'], 120),
    beatsPerBar: (j['beatsPerBar'] as num?)?.toInt() ?? 4,
    loopOn: loop['on'] == true,
    loopStart: _d(loop['start']),
    loopEnd: _d(loop['end'], 16),
    startBeat: _d(j['start']),
    clockPorts: {for (final c in (j['clockTo'] as List? ?? const [])) c as String},
    click: _readClick(j['click'] as Map<String, dynamic>? ?? const {}),
    countIn: (j['countIn'] as num?)?.toInt() ?? 1,
    tracks: [for (final t in (j['tracks'] as List? ?? const [])) _readTrack(t as Map<String, dynamic>)],
  );
}

Click _readClick(Map<String, dynamic> j) => Click(
    port: j['port'] as String?,
    channel: (j['channel'] as num?)?.toInt() ?? 9,
    note: (j['note'] as num?)?.toInt() ?? 37,
    on: j['on'] == true);

Map<String, int> _ints(Object? v) => {
      for (final e in ((v as Map<String, dynamic>?) ?? const {}).entries)
        if (e.value is num) e.key: (e.value as num).toInt()
    };

int _id(Object? v) {
  final id = (v as num?)?.toInt() ?? newId();
  seenId(id);
  return id;
}

Track _readTrack(Map<String, dynamic> j) {
  final t = Track(j['name'] as String? ?? 'track',
      id: _id(j['id']),
      port: j['port'] as String?,
      channel: (j['channel'] as num?)?.toInt() ?? 0,
      mpe: j['mpe'] == true,
      input: j['input'] as String?,
      device: j['device'] as Map<String, dynamic>?,
      box: j['box'] as String?,
      program: (j['program'] as num?)?.toInt(),
      bankMsb: ((j['bank'] as List?)?.elementAtOrNull(0) as num?)?.toInt(),
      bankLsb: ((j['bank'] as List?)?.elementAtOrNull(1) as num?)?.toInt(),
      state: _ints(j['state']),
      clips: [for (final c in (j['clips'] as List? ?? const [])) _readClip(c as Map<String, dynamic>)]);
  t
    ..monitor = j['monitor'] != false
    ..mute = j['mute'] == true
    ..solo = j['solo'] == true;
  return t;
}

Clip _readClip(Map<String, dynamic> j) {
  final Content content;
  if (j['pattern'] case final Map<String, dynamic> p) {
    content = _readPattern(p);
  } else {
    content = _readMidi(j['midi'] as Map<String, dynamic>? ?? const {});
  }
  return Clip(j['name'] as String? ?? 'clip', _d(j['start']), _d(j['length'], 4), content,
      offset: _d(j['offset']), loops: j['loops'] != false, id: _id(j['id']));
}

MidiContent _readMidi(Map<String, dynamic> j) => MidiContent(_d(j['length'], 4), notes: [
      for (final n in (j['notes'] as List? ?? const []))
        if (n case final Map<String, dynamic> n)
          Note(_d(n['t']), _d(n['len'], 0.25), (n['pitch'] as num).toInt(),
              velocity: (n['vel'] as num?)?.toInt(),
              release: (n['rel'] as num?)?.toInt() ?? 0,
              expression: {
                for (final e in ((n['expr'] as Map<String, dynamic>?) ?? const {}).entries) e.key: _readPoints(e.value)
              })
    ], lanes: {
      for (final l in ((j['lanes'] as Map<String, dynamic>?) ?? const {}).entries) l.key: _readPoints(l.value)
    }, other: [
      for (final r in (j['other'] as List? ?? const []))
        RawEvent(_d((r as List)[0]), [for (final b in r[1] as List) (b as num).toInt()])
    ]);

Pattern _readPattern(Map<String, dynamic> j) {
  final rows = <PatternRow>[], cells = <List<Step?>>[];
  for (final r in (j['rows'] as List? ?? const [])) {
    final m = r as Map<String, dynamic>;
    rows.add(PatternRow((m['note'] as num).toInt(), m['name'] as String? ?? '', mute: m['mute'] == true));
    final row = List<Step?>.filled(kMaxSteps, null);
    for (final c in (m['cells'] as List? ?? const [])) {
      final s = c as Map<String, dynamic>, i = (s['i'] as num).toInt();
      if (i < 0 || i >= kMaxSteps) continue;
      row[i] = Step(
          velocity: (s['vel'] as num?)?.toInt(),
          gate: _d(s['gate'], _defaultStep.gate),
          probability: _d(s['prob'], 1),
          nudge: _d(s['nudge']),
          repeat: (s['repeat'] as num?)?.toInt() ?? 1,
          locks: _ints(s['locks']));
    }
    cells.add(row);
  }
  return Pattern(rows,
      steps: (j['steps'] as num?)?.toInt() ?? 16,
      stepBeats: _d(j['stepBeats'], 0.25),
      swing: _d(j['swing'], 0.5),
      cells: cells);
}

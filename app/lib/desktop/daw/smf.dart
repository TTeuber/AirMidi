// Standard MIDI Files (docs/daw.md "Projects and files").
//
// Export: type 1, a tempo track (tempo, time signature) and a track per audible track. The events come from the
// same compiler that plays the arrangement, timed in ticks instead of nanoseconds, so a file holds exactly what play
// sends: patterns as notes (swing, nudge, repeats, the first pass's probabilities), MIDI clips cut and repeated as
// placed, MPE tracks on member channels with their expression and an MPE Configuration Message first.
//
// Import: type 0 or 1, ticks per quarter note (SMPTE timing is refused). A file track with an MPE Configuration
// Message or notes on three or more channels becomes one MPE track; any other track becomes a track per channel it
// uses. Each gets one MIDI clip from
// bar 1, as long as its last event (whole bars). Only the first tempo and time signature are read: the DAW has one
// tempo, and clips are in beats, so the notes keep their musical places either way.
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/compile.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/record.dart';
import 'package:airmidi/desktop/daw/time.dart';

const kPpq = 480;

List<int> _vlq(int v) {
  final out = <int>[v & 0x7F];
  v >>= 7;
  while (v > 0) {
    out.insert(0, 0x80 | (v & 0x7F));
    v >>= 7;
  }
  return out;
}

List<int> _u32(int v) => [(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF];

List<int> _chunk(String id, List<int> data) => [...id.codeUnits, ..._u32(data.length), ...data];

List<int> _meta(int type, List<int> data) => [0xFF, type, ..._vlq(data.length), ...data];

/// [events] (tick, message) as a track's body: delta times, metas and SysEx in their file form, an end of track.
List<int> _track(List<(int, List<int>)> events, {String? name}) {
  final out = <int>[];
  if (name != null) out.addAll([0, ..._meta(0x03, name.codeUnits.map((c) => c & 0x7F).toList())]);
  var last = 0;
  for (final (t, m) in events) {
    out.addAll(_vlq(math.max(0, t - last)));
    last = math.max(last, t);
    if (m.first == 0xF0) {
      out.addAll([0xF0, ..._vlq(m.length - 1), ...m.sublist(1)]);
    } else {
      out.addAll(m);
    }
  }
  out.addAll([0, 0xFF, 0x2F, 0]);
  return out;
}

/// The song's length in beats: to the end of its last clip, whole bars, at least one.
double songLength(Project p) {
  var end = 0.0;
  for (final t in p.tracks) {
    for (final c in t.clips) {
      end = math.max(end, c.end);
    }
  }
  final bar = p.beatsPerBar.toDouble();
  return math.max(bar, (end / bar).ceil() * bar);
}

List<int> exportSmf(Project p, {int ppq = kPpq}) {
  final end = songLength(p);
  final usPerBeat = (60e6 / p.bpm).round();
  final tracks = <List<int>>[
    _track([
      (0, _meta(0x51, [(usPerBeat >> 16) & 0xFF, (usPerBeat >> 8) & 0xFF, usPerBeat & 0xFF])),
      (0, _meta(0x58, [p.beatsPerBar, 2, 24, 8])), // n/4
    ], name: 'airmidi'),
  ];
  for (final t in p.audible) {
    // the track on its own, sending somewhere, so the compiler plays it
    final one = t.copy(keepId: true)
      ..port = 'smf'
      ..mute = false
      ..solo = false;
    final ev = compile(Project(beatsPerBar: p.beatsPerBar, tracks: [one]), 0, end, (b) => (b * ppq).round(), cut: end);
    sortEvents(ev);
    final events = <(int, List<int>)>[
      // lower zone, 15 member channels: RPN 6 on the manager channel
      if (t.mpe) ...[
        (0, [0xB0, 101, 0]),
        (0, [0xB0, 100, 6]),
        (0, [0xB0, 6, 15])
      ],
      for (final e in ev) (e.tNs, e.bytes),
    ];
    tracks.add(_track(events, name: t.name));
  }
  return [
    ..._chunk('MThd', [0, 1, (tracks.length >> 8) & 0xFF, tracks.length & 0xFF, (ppq >> 8) & 0xFF, ppq & 0xFF]),
    for (final t in tracks) ..._chunk('MTrk', t),
  ];
}

class SmfImport {
  final double? bpm;
  final int? beatsPerBar;
  final List<Track> tracks;
  const SmfImport(this.bpm, this.beatsPerBar, this.tracks);
}

SmfImport importSmf(List<int> data, {int beatsPerBar = 4}) {
  var i = 0;
  int u16() => (data[i++] << 8) | data[i++];
  int u32() => (u16() << 16) | u16();
  String id() => String.fromCharCodes(data.sublist(i, i += 4));
  if (data.length < 14 || id() != 'MThd') throw const FormatException('not a MIDI file');
  final hlen = u32(), start = i;
  u16(); // format: 0 and 1 read the same way here, one track or several
  final ntrks = u16(), division = u16();
  if (division & 0x8000 != 0) throw const FormatException('SMPTE timing is not supported');
  i = start + hlen;
  double? bpm;
  int? bpb;
  final out = <Track>[];
  for (var n = 0; n < ntrks && i + 8 <= data.length; n++) {
    final cid = id(), len = u32(), end = math.min(data.length, i + len);
    if (cid != 'MTrk') {
      i = end;
      continue;
    }
    String? name;
    final raw = <(double, List<int>)>[];
    var tick = 0, status = 0;
    int vlq() {
      var v = 0;
      while (i < end) {
        final b = data[i++];
        v = (v << 7) | (b & 0x7F);
        if (b & 0x80 == 0) break;
      }
      return v;
    }

    while (i < end) {
      tick += vlq();
      if (i >= end) break;
      var b = data[i];
      if (b == 0xFF) {
        i++;
        final type = data[i++], l = vlq(), body = data.sublist(i, math.min(end, i + l));
        i += l;
        if (type == 0x03 && name == null) name = String.fromCharCodes(body).trim();
        if (type == 0x51 && bpm == null && body.length == 3) bpm = 60e6 / ((body[0] << 16) | (body[1] << 8) | body[2]);
        if (type == 0x58 && bpb == null && body.isNotEmpty) bpb = body[0];
        if (type == 0x2F) break;
        continue;
      }
      if (b == 0xF0 || b == 0xF7) {
        i++;
        final l = vlq();
        i += l; // SysEx is not kept on import
        continue;
      }
      if (b & 0x80 != 0) {
        status = b;
        i++;
      } else if (status == 0) {
        i++; // data byte with no status: skip it
        continue;
      }
      final size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
      if (i + size > end) break;
      raw.add((tick / division, [status, ...data.sublist(i, i + size)]));
      i += size;
    }
    i = end;
    if (raw.isEmpty) continue;
    final bar = (bpb ?? beatsPerBar).toDouble();
    final last = raw.map((e) => e.$1).reduce(math.max);
    final length = math.max(bar, ((last + 1e-6) / bar).ceil() * bar);
    Track make(String title, List<(double, List<int>)> events, {required int channel, required bool mpe}) {
      final m = MidiContent(length);
      addPlayed(m, events, mpe: mpe, now: last, x: (b) => b);
      return Track(title, channel: channel, mpe: mpe, clips: [Clip(title, 0, length, m)]);
    }

    final base = name == null || name.isEmpty ? 'track ${out.length + 1}' : name;
    // MPE: an MPE Configuration Message (RPN 6 on channel 1), or notes on three channels or more
    var mcm = false;
    for (var k = 0; k + 1 < raw.length; k++) {
      final a = raw[k].$2, b = raw[k + 1].$2;
      if (a[0] == 0xB0 && a[1] == 101 && a[2] == 0 && b[0] == 0xB0 && b[1] == 100 && b[2] == 6) mcm = true;
    }
    if (mcm || noteChannels(raw) >= 3) {
      // the setup itself is not kept: played again at the clip's start it would cut the synth's notes
      out.add(make(
          base,
          [
            for (final e in raw)
              if (!(e.$2[0] == 0xB0 && const [100, 101, 6, 38].contains(e.$2[1]))) e
          ],
          channel: 0,
          mpe: true));
      continue;
    }
    final channels = {for (final (_, m) in raw) m[0] & 0x0F}.toList()..sort();
    for (final ch in channels) {
      final title = channels.length == 1 ? (name == null && ch == 9 ? 'drums' : base) : '$base ch ${ch + 1}';
      out.add(make(
          title,
          [
            for (final e in raw)
              if (e.$2[0] & 0x0F == ch) e
          ],
          channel: ch,
          mpe: false));
    }
  }
  return SmfImport(bpm, bpb, out);
}

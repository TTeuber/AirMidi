// The tracks page (docs/desktop.md D3, D4): one transport, step tracks, clip tracks, MIDI clock out. A track sends
// to one of the computer's MIDI outputs; a click on a cell toggles a step; an armed clip track records its input
// while playing ("keys" opens the computer keyboard as an input). The song and its player live as long as
// the app (not saved yet), so leaving the page does not stop play.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../link/desktop_midi.dart';
import '../tracks/keys_bus.dart';
import '../tracks/player.dart';
import '../tracks/sequencer.dart';
import 'keys_page.dart';
import 'strips_page.dart';

final _song = Song(tracks: [drumTrack()]);
SeqPlayer? _player;

class TracksPage extends StatefulWidget {
  const TracksPage({super.key});

  @override
  State<TracksPage> createState() => _TracksPageState();
}

class _TracksPageState extends State<TracksPage> with SingleTickerProviderStateMixin {
  late final SeqPlayer player = _player ??= SeqPlayer(_song, DesktopMidi.engine);
  late final Ticker ticker = createTicker((_) => setState(() {})); // the playhead
  Song get song => _song;
  StreamSubscription<void>? _ports;

  @override
  void initState() {
    super.initState();
    player.addListener(_changed);
    _ports = DesktopMidi.changes.listen((_) => setState(() {}));
    if (player.playing) ticker.start();
  }

  void _changed() {
    if (player.playing && !ticker.isActive) ticker.start();
    if (!player.playing && ticker.isActive) ticker.stop();
    setState(() {});
  }

  @override
  void dispose() {
    player.removeListener(_changed);
    _ports?.cancel();
    ticker.dispose();
    super.dispose();
  }

  List<String> get portNames => [for (final p in DesktopMidi.outputs()) p.name];

  // What a clip track can record: the computer's inputs (not the app's own output) and the keys page.
  List<String> get inputNames => [
        kKeysInput,
        for (final p in DesktopMidi.engine.inputs)
          if (p.name != kVirtualName) p.name,
      ];

  Widget _dropdown(String hint, String? value, List<String> names, ValueChanged<String?> set) =>
      DropdownButton<String?>(
        value: names.contains(value) ? value : null,
        hint: Text(hint, style: const TextStyle(color: cGrey, fontSize: 13)),
        dropdownColor: cBg,
        style: const TextStyle(color: cN, fontSize: 13),
        items: [for (final n in names) DropdownMenuItem(value: n, child: Text(n))],
        onChanged: set,
      );

  Widget _clip(ClipTrack t, double beat) {
    final pos = player.playing && beat >= 0 ? (beat % t.lengthBeats) / t.lengthBeats : -1.0;
    return Container(
      margin: const EdgeInsets.only(bottom: 18),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Wrap(spacing: 12, crossAxisAlignment: WrapCrossAlignment.center, children: [
          Text(t.name, style: const TextStyle(color: cAc, fontSize: 14)),
          _dropdown('record from', t.input, inputNames, (v) {
            setState(() => t.input = v);
            player.syncInputs();
          }),
          _dropdown('send to', t.port, portNames, (v) => setState(() => t.port = v)),
          _stepper(
              'bars',
              '${t.lengthBeats ~/ 4}',
              () => setState(() => t.lengthBeats = (t.lengthBeats - 4).clamp(4, 64)),
              () => setState(() => t.lengthBeats = (t.lengthBeats + 4).clamp(4, 64))),
          _chip('rec', t.armed, () {
            setState(() => t.armed = !t.armed);
            player.syncInputs();
          }),
          _chip('clear', false, () => setState(() => t.events.clear())),
          _chip('mute', t.mute, () => setState(() => t.mute = !t.mute)),
          _chip(
              'remove',
              false,
              () => setState(() {
                    t.armed = false;
                    song.tracks.remove(t);
                    player.syncInputs();
                  })),
          Text('${t.events.length} events', style: const TextStyle(color: cGrey, fontSize: 12)),
        ]),
        const SizedBox(height: 6),
        SizedBox(
          height: 110,
          child: CustomPaint(size: Size.infinite, painter: _ClipPainter(t, pos, t.armed && player.playing)),
        ),
      ]),
    );
  }

  @override
  Widget build(BuildContext context) {
    final beat = player.beat;
    return Scaffold(
      appBar: AppBar(title: const Text('tracks'), backgroundColor: cBg),
      body: ListView(padding: const EdgeInsets.all(14), children: [
        _transport(beat),
        if (player.error != null) Text(player.error!, style: const TextStyle(color: Color(0xFFAB6D6C))),
        const SizedBox(height: 14),
        for (final t in song.tracks)
          if (t is StepTrack) _track(t, beat) else if (t is ClipTrack) _clip(t, beat),
        Row(children: [
          _button('+ drums', () => setState(() => song.tracks.add(drumTrack()))),
          const SizedBox(width: 8),
          _button('+ notes', () => setState(() => song.tracks.add(noteTrack()))),
          const SizedBox(width: 8),
          _button('+ clip', () => setState(() => song.tracks.add(ClipTrack()))),
        ]),
      ]),
    );
  }

  Widget _transport(double beat) {
    final bar = beat < 0 ? '' : '${beat ~/ 4 + 1}.${beat.floor() % 4 + 1}';
    return Wrap(spacing: 14, runSpacing: 8, crossAxisAlignment: WrapCrossAlignment.center, children: [
      _button(player.playing ? 'stop' : 'play', () => player.playing ? player.stop() : player.play(),
          lit: player.playing, width: 90),
      Row(mainAxisSize: MainAxisSize.min, children: [
        const Text('bpm', style: TextStyle(color: cGrey, fontSize: 13)),
        IconButton(onPressed: () => player.setBpm(song.bpm - 1), icon: const Icon(Icons.remove, size: 16), color: cAc),
        SizedBox(
            width: 40,
            child: Center(child: Text(song.bpm.round().toString(), style: const TextStyle(color: cN, fontSize: 16)))),
        IconButton(onPressed: () => player.setBpm(song.bpm + 1), icon: const Icon(Icons.add, size: 16), color: cAc),
      ]),
      SizedBox(width: 48, child: Text(bar, style: const TextStyle(color: cN, fontSize: 16))),
      _button(
          'keys', () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => KeysPage(link: KeysBusLink())))),
      const Text('clock to', style: TextStyle(color: cGrey, fontSize: 13)),
      for (final p in portNames)
        _chip(p, song.clockPorts.contains(p), () {
          setState(() => song.clockPorts.contains(p) ? song.clockPorts.remove(p) : song.clockPorts.add(p));
        }),
    ]);
  }

  Widget _track(StepTrack t, double beat) {
    final names = portNames;
    return Container(
      margin: const EdgeInsets.only(bottom: 18),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Wrap(spacing: 12, crossAxisAlignment: WrapCrossAlignment.center, children: [
          Text(t.name, style: const TextStyle(color: cAc, fontSize: 14)),
          DropdownButton<String?>(
            value: names.contains(t.port) ? t.port : null,
            hint: const Text('send to', style: TextStyle(color: cGrey, fontSize: 13)),
            dropdownColor: cBg,
            style: const TextStyle(color: cN, fontSize: 13),
            items: [for (final n in names) DropdownMenuItem(value: n, child: Text(n))],
            onChanged: (v) => setState(() => t.port = v),
          ),
          _stepper('ch', '${t.channel + 1}', () => setState(() => t.channel = (t.channel - 1) % 16),
              () => setState(() => t.channel = (t.channel + 1) % 16)),
          _stepper('steps', '${t.steps}', () => setState(() => t.steps = (t.steps - 1).clamp(1, 64)),
              () => setState(() => t.steps = (t.steps + 1).clamp(1, 64))),
          _chip('mute', t.mute, () => setState(() => t.mute = !t.mute)),
          _chip('remove', false, () => setState(() => song.tracks.remove(t))),
        ]),
        const SizedBox(height: 6),
        LayoutBuilder(builder: (context, box) {
          const labelW = 64.0;
          final cell = ((box.maxWidth - labelW) / t.steps).clamp(10.0, 34.0);
          final playing = player.playing ? stepAt(t, song.stepsPerBeat, beat) : -1;
          return Column(children: [
            for (var r = 0; r < t.rows.length; r++)
              Row(children: [
                SizedBox(width: labelW, child: Text(t.rows[r].label, style: const TextStyle(color: cN, fontSize: 12))),
                for (var s = 0; s < t.steps; s++) _cell(t, r, s, cell, s == playing),
              ]),
          ]);
        }),
      ]),
    );
  }

  Widget _cell(StepTrack t, int r, int s, double size, bool playhead) {
    final on = t.on(r, s), beatStart = s % song.stepsPerBeat == 0;
    final hue = stripHues[(s ~/ song.stepsPerBeat) % 3];
    return GestureDetector(
      onTap: () => setState(() => t.toggle(r, s)),
      child: Container(
        width: size,
        height: 24,
        margin: EdgeInsets.only(left: beatStart && s > 0 ? 3 : 1, bottom: 1),
        color: on
            ? (playhead ? cAc : hue.drag)
            : (playhead ? cAc.withValues(alpha: 0.25) : hue.base.withValues(alpha: beatStart ? 0.28 : 0.16)),
      ),
    );
  }

  Widget _stepper(String label, String value, VoidCallback down, VoidCallback up) =>
      Row(mainAxisSize: MainAxisSize.min, children: [
        Text(label, style: const TextStyle(color: cGrey, fontSize: 12)),
        IconButton(
            onPressed: down,
            icon: const Icon(Icons.remove, size: 14),
            color: cAc,
            visualDensity: VisualDensity.compact),
        Text(value, style: const TextStyle(color: cN, fontSize: 13)),
        IconButton(
            onPressed: up, icon: const Icon(Icons.add, size: 14), color: cAc, visualDensity: VisualDensity.compact),
      ]);

  Widget _chip(String label, bool on, VoidCallback tap) => GestureDetector(
        onTap: tap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(color: on ? cAc : null, border: Border.all(color: on ? cAc : cGrey)),
          child: Text(label, style: TextStyle(color: on ? cBg : cN, fontSize: 12)),
        ),
      );

  Widget _button(String label, VoidCallback tap, {bool lit = false, double width = 110}) => GestureDetector(
        onTap: tap,
        child: Container(
          width: width,
          height: 38,
          alignment: Alignment.center,
          decoration: BoxDecoration(color: lit ? cAc : null, border: Border.all(color: cAc, width: 1.5)),
          child: Text(label, style: TextStyle(color: lit ? cBg : cAc, fontSize: 14)),
        ),
      );
}

/// A clip as a piano roll: each note a bar at its pitch (the recorded range fills the height), coloured by channel
/// (MPE: a strip each), the beats as lines, the playhead.
class _ClipPainter extends CustomPainter {
  final ClipTrack t;
  final double pos; // 0..1, or < 0 when stopped
  final bool recording;
  _ClipPainter(this.t, this.pos, this.recording);

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    canvas.drawRect(Offset.zero & size,
        Paint()..color = (recording ? const Color(0xFFAB6D6C) : stripHues[0].base).withValues(alpha: 0.12));
    final beats = t.lengthBeats.round();
    for (var b = 0; b <= beats; b++) {
      canvas.drawLine(Offset(w * b / beats, 0), Offset(w * b / beats, h),
          Paint()..color = cGrey.withValues(alpha: b % 4 == 0 ? 0.45 : 0.15));
    }
    final notes = t.notes();
    if (notes.isNotEmpty) {
      var lo = 127, hi = 0;
      for (final n in notes) {
        lo = n.$2 < lo ? n.$2 : lo;
        hi = n.$2 > hi ? n.$2 : hi;
      }
      final rows = (hi - lo + 1).clamp(6, 48), rowH = h / rows;
      for (final (ch, note, s, e) in notes) {
        final y = h - (note - lo + 1) * rowH;
        final r = Rect.fromLTRB(
            w * s / t.lengthBeats, y, (w * e / t.lengthBeats).clamp(w * s / t.lengthBeats + 2, w), y + rowH - 1);
        canvas.drawRect(r, Paint()..color = stripHues[ch % 3].drag);
      }
    }
    if (pos >= 0)
      canvas.drawLine(
          Offset(w * pos, 0),
          Offset(w * pos, h),
          Paint()
            ..color = cAc
            ..strokeWidth = 2);
  }

  @override
  bool shouldRepaint(_ClipPainter old) => true;
}

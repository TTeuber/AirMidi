// The DAW's window (docs/daw.md "The window"): the transport, track headers down the left, the arrangement
// (timeline.dart), and the bottom panel with the selected clip in its editor: the piano roll for a MIDI clip, the
// sequencer for a pattern clip. The project, its undo history and the player live as long as the app (not saved
// yet, A4), so leaving the page does not stop play.
//
// Keys: space play/stop, R record, L loop, cmd-Z / shift-cmd-Z undo/redo, delete, cmd-D duplicate, cmd-C / cmd-V
// copy and paste (at the start marker, on the selected clip's track), cmd-T split at the playhead. After a click in
// the bottom panel the editor gets the keys first (the piano roll's delete removes notes, not the clip).
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/keys_bus.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/pattern_editor.dart';
import 'package:airmidi/desktop/daw/piano_roll.dart';
import 'package:airmidi/desktop/daw/player.dart';
import 'package:airmidi/desktop/daw/timeline.dart';
import 'package:airmidi/desktop/desktop_midi.dart';
import 'package:airmidi/desktop/keys_page.dart';
import 'package:airmidi/shared/ui/theme.dart';

final _project = Project(tracks: [
  Track('drums', channel: 9, clips: [Clip('beat', 0, 4, Pattern(gmDrumRows()))]),
  Track('synth'),
]);
final _history = History(_project);
DawPlayer? _player;
Clip? _clipboard;

const kHeaderW = 230.0;

class ArrangePage extends StatefulWidget {
  const ArrangePage({super.key});

  @override
  State<ArrangePage> createState() => _ArrangePageState();
}

class _ArrangePageState extends State<ArrangePage> with SingleTickerProviderStateMixin {
  late final DawPlayer player = _player ??= DawPlayer(_project, _history, DesktopMidi.engine);
  late final Ticker ticker = createTicker((_) => setState(() {})); // the playhead
  final view = ArrangeView();
  final focus = FocusNode();
  StreamSubscription<void>? _ports;
  Clip? selected;
  double panelH = 330;
  bool editorKeys = false; // the last click was in the bottom panel
  GlobalKey<PianoRollState> rollKey = GlobalKey();
  int? _rollFor; // the clip the roll's state belongs to

  Project get p => _project;
  History get history => _history;

  @override
  void initState() {
    super.initState();
    player.addListener(_changed);
    view.addListener(_changed);
    _ports = DesktopMidi.changes.listen((_) => setState(() {}));
    if (player.playing) ticker.start();
  }

  void _changed() {
    if (player.playing && !ticker.isActive) ticker.start();
    if (!player.playing && ticker.isActive) ticker.stop();
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    player.removeListener(_changed);
    view.dispose();
    _ports?.cancel();
    ticker.dispose();
    focus.dispose();
    super.dispose();
  }

  void _edit(void Function() f) {
    history.mark();
    setState(f);
  }

  // After undo the clips are copies: find the selected one again by its id.
  void _reselect() {
    final id = selected?.id;
    selected = null;
    for (final t in p.tracks) {
      for (final c in t.clips) {
        if (c.id == id) selected = c;
      }
    }
  }

  void _undo() => setState(() {
        if (history.undo()) _reselect();
        player.syncInputs();
      });

  void _redo() => setState(() {
        if (history.redo()) _reselect();
        player.syncInputs();
      });

  // ---- clip operations

  Clip _newClip(Track t, double beat, {required bool pattern}) {
    final bpb = p.beatsPerBar.toDouble(), at = math.max(0.0, (beat / bpb).floor() * bpb);
    final Content content = pattern ? Pattern(t.channel == 9 ? gmDrumRows() : chromaticRows(48)) : MidiContent(bpb);
    final c = Clip(pattern ? 'pattern' : 'clip', at, pattern ? content.length : bpb, content);
    _edit(() {
      t.clips.add(c);
      selected = c;
    });
    return c;
  }

  void _duplicate(Clip c) {
    final t = p.trackOf(c);
    if (t == null) return;
    final copy = c.copy()..start = c.end;
    _edit(() {
      t.clips.add(copy);
      selected = copy;
    });
  }

  void _delete(Clip c) => _edit(() {
        p.trackOf(c)?.clips.remove(c);
        if (identical(selected, c)) selected = null;
      });

  void _split(Clip c, double at) {
    if (at <= c.start + 1e-6 || at >= c.end - 1e-6) return;
    final t = p.trackOf(c)!, len = c.content.length;
    _edit(() {
      final second = c.copy()
        ..start = at
        ..length = c.end - at
        ..offset = len > 0 ? (c.offset + (at - c.start)) % len : 0;
      c.length = at - c.start;
      t.clips.add(second);
      selected = second;
    });
  }

  void _paste(Track t, double at) {
    final c = _clipboard;
    if (c == null) return;
    final copy = c.copy()..start = at;
    _edit(() {
      t.clips.add(copy);
      selected = copy;
    });
  }

  Future<void> _rename(String title, String now, void Function(String) set) async {
    final ctl = TextEditingController(text: now);
    final name = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        backgroundColor: cBg,
        title: Text(title, style: const TextStyle(color: cN, fontSize: 15)),
        content: TextField(controller: ctl, autofocus: true, onSubmitted: (v) => Navigator.of(context).pop(v)),
        actions: [TextButton(onPressed: () => Navigator.of(context).pop(ctl.text), child: const Text('ok'))],
      ),
    );
    if (name != null && name.trim().isNotEmpty) _edit(() => set(name.trim()));
    focus.requestFocus();
  }

  Future<T?> _menu<T>(Offset at, List<(String, T)> items) => showMenu<T>(
        context: context,
        color: cBg,
        position: RelativeRect.fromLTRB(at.dx, at.dy, at.dx + 1, at.dy + 1),
        items: [
          for (final (label, v) in items)
            PopupMenuItem(value: v, height: 34, child: Text(label, style: const TextStyle(color: cN, fontSize: 13))),
        ],
      );

  Future<void> _emptyMenu(Track t, double beat, Offset at) async {
    final r = await _menu(at, [
      ('new MIDI clip', 'midi'),
      ('new pattern clip', 'pattern'),
      if (_clipboard != null) ('paste', 'paste'),
    ]);
    final bpb = p.beatsPerBar.toDouble();
    if (r == 'midi') _newClip(t, beat, pattern: false);
    if (r == 'pattern') _newClip(t, beat, pattern: true);
    if (r == 'paste') _paste(t, (beat / bpb).floor() * bpb);
    focus.requestFocus();
  }

  Future<void> _clipMenu(Clip c, Offset at) async {
    final r = await _menu(at, [
      ('rename', 'rename'),
      ('duplicate', 'dup'),
      ('split at the playhead', 'split'),
      ('copy', 'copy'),
      ('delete', 'delete'),
    ]);
    switch (r) {
      case 'rename':
        await _rename('clip name', c.name, (v) => c.name = v);
      case 'dup':
        _duplicate(c);
      case 'split':
        _split(c, player.beat);
      case 'copy':
        _clipboard = c.copy();
      case 'delete':
        _delete(c);
    }
    focus.requestFocus();
  }

  Future<void> _trackMenu(Track t, Offset at) async {
    final i = p.tracks.indexOf(t);
    final r = await _menu(at, [
      ('rename', 'rename'),
      if (i > 0) ('move up', 'up'),
      if (i < p.tracks.length - 1) ('move down', 'down'),
      ('remove', 'remove'),
    ]);
    switch (r) {
      case 'rename':
        await _rename('track name', t.name, (v) => t.name = v);
      case 'up':
        _edit(() => p.tracks.insert(i - 1, p.tracks.removeAt(i)));
      case 'down':
        _edit(() => p.tracks.insert(i + 1, p.tracks.removeAt(i)));
      case 'remove':
        _edit(() {
          p.tracks.remove(t);
          if (selected != null && !t.clips.contains(selected)) return;
          selected = null;
        });
        player.syncInputs();
    }
    focus.requestFocus();
  }

  // ---- keys

  KeyEventResult _key(FocusNode node, KeyEvent e) {
    if (e is KeyRepeatEvent && editorKeys && rollKey.currentState?.key(e) == true) return KeyEventResult.handled;
    if (e is! KeyDownEvent) return KeyEventResult.ignored;
    final k = e.logicalKey, keys = HardwareKeyboard.instance;
    final cmd = keys.isMetaPressed || keys.isControlPressed, shift = keys.isShiftPressed;
    final c = selected;
    if (editorKeys && rollKey.currentState?.key(e) == true) return KeyEventResult.handled;
    if (k == LogicalKeyboardKey.space) {
      player.playing ? player.stop() : player.play();
    } else if (k == LogicalKeyboardKey.keyR && !cmd) {
      player.playing ? player.setRecording(!player.recording) : player.play(record: true);
    } else if (k == LogicalKeyboardKey.keyL && !cmd) {
      setState(() => p.loopOn = !p.loopOn);
    } else if (cmd && k == LogicalKeyboardKey.keyZ) {
      shift ? _redo() : _undo();
    } else if (c != null && (k == LogicalKeyboardKey.backspace || k == LogicalKeyboardKey.delete)) {
      _delete(c);
    } else if (c != null && cmd && k == LogicalKeyboardKey.keyD) {
      _duplicate(c);
    } else if (c != null && cmd && k == LogicalKeyboardKey.keyC) {
      _clipboard = c.copy();
    } else if (cmd && k == LogicalKeyboardKey.keyV) {
      final t = c != null ? p.trackOf(c) : (p.tracks.isEmpty ? null : p.tracks.first);
      if (t != null) _paste(t, p.startBeat);
    } else if (c != null && cmd && k == LogicalKeyboardKey.keyT) {
      _split(c, player.beat);
    } else {
      return KeyEventResult.ignored;
    }
    return KeyEventResult.handled;
  }

  // ---- build

  @override
  Widget build(BuildContext context) {
    final playhead = player.playing ? player.beat : -1.0;
    return Focus(
      focusNode: focus,
      autofocus: true,
      onKeyEvent: _key,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('arrangement', style: TextStyle(fontSize: 16)),
          backgroundColor: cBg,
          actions: [
            TextButton(onPressed: history.canUndo ? _undo : null, child: const Text('undo')),
            TextButton(onPressed: history.canRedo ? _redo : null, child: const Text('redo')),
          ],
        ),
        body: Listener(
          onPointerDown: (_) => focus.requestFocus(),
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            _transport(playhead),
            if (player.error != null)
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 14),
                child: Text(player.error!, style: const TextStyle(color: Color(0xFFAB6D6C), fontSize: 12)),
              ),
            Expanded(
              child: Listener(
                onPointerDown: (_) => editorKeys = false,
                child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                  SizedBox(width: kHeaderW, child: _headers()),
                  Expanded(
                    child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                      Ruler(project: p, view: view, playhead: () => playhead, changed: () => setState(() {})),
                      Expanded(
                        child: Lanes(
                          project: p,
                          history: history,
                          view: view,
                          selected: selected,
                          recording: {for (final t in player.takes.values) t.clip},
                          playhead: () => playhead,
                          changed: () => setState(() {}),
                          actions: ArrangeActions(
                            select: (c) => setState(() => selected = c),
                            open: (c) => setState(() {
                              selected = c;
                              panelH = math.max(panelH, 220);
                            }),
                            emptyMenu: _emptyMenu,
                            clipMenu: _clipMenu,
                          ),
                        ),
                      ),
                    ]),
                  ),
                ]),
              ),
            ),
            GestureDetector(
              behavior: HitTestBehavior.opaque,
              onVerticalDragUpdate: (d) => setState(() => panelH = (panelH - d.delta.dy).clamp(80.0, 700.0)),
              child: MouseRegion(
                cursor: SystemMouseCursors.resizeRow,
                child: Container(height: 7, color: const Color(0xFF1C2329)),
              ),
            ),
            SizedBox(
              height: panelH,
              child: Listener(onPointerDown: (_) => editorKeys = true, child: _panel(playhead)),
            ),
          ]),
        ),
      ),
    );
  }

  Widget _transport(double playhead) {
    final b = player.beat, bpb = p.beatsPerBar;
    final pos = '${b ~/ bpb + 1}.${b.floor() % bpb + 1}.${(b * 4).floor() % 4 + 1}';
    Widget button(String label, VoidCallback tap, {bool lit = false, Color? litColor}) => GestureDetector(
          onTap: tap,
          child: Container(
            width: 64,
            height: 32,
            alignment: Alignment.center,
            decoration: BoxDecoration(
                color: lit ? (litColor ?? cAc) : null, border: Border.all(color: litColor ?? cAc, width: 1.5)),
            child: Text(label, style: TextStyle(color: lit ? cBg : (litColor ?? cAc), fontSize: 13)),
          ),
        );
    const red = Color(0xFFD9574F);
    return Padding(
      padding: const EdgeInsets.fromLTRB(14, 8, 14, 8),
      child: Wrap(spacing: 10, runSpacing: 6, crossAxisAlignment: WrapCrossAlignment.center, children: [
        button(player.playing ? 'stop' : 'play', () => player.playing ? player.stop() : player.play(),
            lit: player.playing),
        button('rec', () => player.playing ? player.setRecording(!player.recording) : player.play(record: true),
            lit: player.recording, litColor: red),
        button('loop', () => setState(() => p.loopOn = !p.loopOn), lit: p.loopOn),
        chip(player.replace ? 'replace' : 'overdub', player.replace,
            () => setState(() => player.replace = !player.replace)),
        stepper('bpm', p.bpm.round().toString(), () => player.setBpm(p.bpm - 1), () => player.setBpm(p.bpm + 1)),
        SizedBox(width: 70, child: Text(pos, style: const TextStyle(color: cN, fontSize: 15))),
        _pick<double>(
            'snap ${_gridName(view.grid)}',
            [
              for (final g in const [4.0, 1.0, 0.5, 0.25, 0.0]) (_gridName(g), g)
            ],
            (g) => setState(() => view.grid = g)),
        stepper('zoom', '', () => view.zoom(1 / 1.4, 0), () => view.zoom(1.4, 0)),
        _pick<String>(
            'clock to ${p.clockPorts.isEmpty ? 'none' : p.clockPorts.length}',
            [
              for (final o in player.outputs) ('${p.clockPorts.contains(o) ? '✓ ' : '   '}$o', o),
            ],
            (o) => setState(() => p.clockPorts.contains(o) ? p.clockPorts.remove(o) : p.clockPorts.add(o))),
        TextButton(
          onPressed: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => KeysPage(link: KeysBusLink()))),
          child: const Text('keys'),
        ),
      ]),
    );
  }

  Widget _roll(Clip c, MidiContent content, double at) {
    if (_rollFor != c.id) {
      rollKey = GlobalKey(); // a new clip: a fresh view, fitted to it
      _rollFor = c.id;
    }
    final t = p.trackOf(c)!, len = content.length;
    final inside = p.startBeat >= c.start && p.startBeat < c.end && len > 0;
    return PianoRoll(
      key: rollKey,
      content: content,
      track: t,
      history: history,
      changed: () => setState(() {}),
      audition: (pitch, on, vel) => player.audition(t, pitch, on, vel),
      playBeat: at,
      pasteAt: inside ? (p.startBeat - c.start + c.offset) % len : 0,
      beatsPerBar: p.beatsPerBar,
    );
  }

  String _gridName(double g) => switch (g) {
        4.0 => 'bar',
        1.0 => 'beat',
        0.5 => '1/8',
        0.25 => '1/16',
        _ => 'off',
      };

  // A word that opens a menu of choices.
  Widget _pick<T>(String label, List<(String, T)> items, void Function(T) set, {Color color = cN, double size = 12}) =>
      Builder(
        builder: (context) => GestureDetector(
          onTapDown: (d) async {
            final v = await _menu<T>(d.globalPosition, items);
            if (v != null) set(v);
            focus.requestFocus();
          },
          child: Text('$label ▾', style: TextStyle(color: color, fontSize: size), overflow: TextOverflow.ellipsis),
        ),
      );

  Widget _headers() => Listener(
        onPointerSignal: (e) {
          if (e is PointerScrollEvent) view.scroll(0, e.scrollDelta.dy, p.tracks.length * kTrackH);
        },
        child: Column(children: [
          const SizedBox(height: kRulerH),
          Expanded(
            child: ClipRect(
              child: OverflowBox(
                alignment: Alignment.topLeft,
                maxHeight: double.infinity,
                child: Transform.translate(
                  offset: Offset(0, -view.top),
                  child: Column(mainAxisSize: MainAxisSize.min, children: [
                    for (var i = 0; i < p.tracks.length; i++) _header(p.tracks[i], i),
                    Padding(
                      padding: const EdgeInsets.all(8),
                      child: Row(children: [
                        chip('+ track', false, () {
                          _edit(() => p.tracks.add(Track('track ${p.tracks.length + 1}')));
                        }),
                      ]),
                    ),
                  ]),
                ),
              ),
            ),
          ),
        ]),
      );

  Widget _header(Track t, int i) {
    const red = Color(0xFFD9574F);
    Widget small(String label, bool on, VoidCallback tap, {Color c = cAc}) => GestureDetector(
          onTap: tap,
          child: Container(
            width: 22,
            height: 20,
            margin: const EdgeInsets.only(left: 3),
            alignment: Alignment.center,
            decoration: BoxDecoration(color: on ? c : null, border: Border.all(color: on ? c : cGrey)),
            child: Text(label, style: TextStyle(color: on ? cBg : cN, fontSize: 11)),
          ),
        );
    return GestureDetector(
      onSecondaryTapDown: (d) => _trackMenu(t, d.globalPosition),
      onDoubleTap: () => _rename('track name', t.name, (v) => t.name = v),
      child: Container(
        height: kTrackH,
        decoration: const BoxDecoration(border: Border(bottom: BorderSide(color: Color(0xFF1C2329)))),
        child: Row(children: [
          Container(width: 5, color: trackHue(i)),
          Expanded(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(8, 5, 6, 4),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [
                  Expanded(
                    child:
                        Text(t.name, style: const TextStyle(color: cN, fontSize: 13), overflow: TextOverflow.ellipsis),
                  ),
                  small('M', t.mute, () => _edit(() => t.mute = !t.mute)),
                  small('S', t.solo, () => _edit(() => t.solo = !t.solo)),
                  small('R', t.armed, () {
                    _edit(() => t.armed = !t.armed);
                    player.syncInputs();
                  }, c: red),
                ]),
                const SizedBox(height: 3),
                Row(children: [
                  Expanded(
                    child: _pick<String>('→ ${t.port ?? 'send to'}', [for (final o in player.outputs) (o, o)],
                        (o) => _edit(() => t.port = o),
                        color: t.port == null ? cGrey : cN, size: 11),
                  ),
                  if (!t.mpe)
                    _pick<int>('ch ${t.channel + 1}', [for (var c = 0; c < 16; c++) ('${c + 1}', c)],
                        (c) => _edit(() => t.channel = c),
                        size: 11),
                  const SizedBox(width: 6),
                  GestureDetector(
                    onTap: () => _edit(() => t.mpe = !t.mpe),
                    child: Text('MPE', style: TextStyle(color: t.mpe ? cAc : cGrey, fontSize: 11)),
                  ),
                ]),
                const SizedBox(height: 3),
                _pick<String>('← ${t.input ?? 'record from'}', [for (final n in player.inputs) (n, n)], (n) {
                  _edit(() => t.input = n);
                  player.syncInputs();
                }, color: t.input == null ? cGrey : cN, size: 11),
              ]),
            ),
          ),
        ]),
      ),
    );
  }

  Widget _panel(double playhead) {
    final c = selected;
    if (c == null) {
      return const Center(
        child: Text('Click a clip to edit it here. Double-click or right-click an empty spot on a track for a new one.',
            style: TextStyle(color: cGrey, fontSize: 13)),
      );
    }
    final len = c.content.length, bpb = p.beatsPerBar;
    // where play is inside this clip's content, if it is in the clip
    final inClip = playhead >= c.start && playhead < c.end && len > 0;
    final at = inClip ? (playhead - c.start + c.offset) % len : -1.0;
    final content = c.content;
    String beats(double b) {
      if (b % bpb == 0) return b == bpb ? '1 bar' : '${(b / bpb).round()} bars';
      return b == 1 ? '1 beat' : '${b.toStringAsFixed(b % 1 == 0 ? 0 : 2)} beats';
    }

    return Padding(
      padding: const EdgeInsets.fromLTRB(14, 8, 14, 10),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Wrap(spacing: 14, crossAxisAlignment: WrapCrossAlignment.center, children: [
          GestureDetector(
            onTap: () => _rename('clip name', c.name, (v) => c.name = v),
            child: Text(c.name, style: const TextStyle(color: cAc, fontSize: 14)),
          ),
          Text(content is Pattern ? 'pattern · sequencer' : 'MIDI · piano roll',
              style: const TextStyle(color: cGrey, fontSize: 12)),
          Text('on the timeline ${beats(c.length)}', style: const TextStyle(color: cGrey, fontSize: 12)),
          if (content is MidiContent)
            stepper('loop', beats(content.length), () => _edit(() => content.length = math.max(1, content.length - 1)),
                () => _edit(() => content.length += 1)),
          if (content is MidiContent)
            Text('${content.notes.length} notes', style: const TextStyle(color: cGrey, fontSize: 12)),
        ]),
        const SizedBox(height: 4),
        Expanded(
          child: content is Pattern
              ? PatternEditor(
                  pattern: content,
                  history: history,
                  changed: () => setState(() {}),
                  playStep: at < 0 ? -1 : (at / content.stepBeats).floor())
              : _roll(c, content as MidiContent, at),
        ),
      ]),
    );
  }
}

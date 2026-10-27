// The piano roll (docs/daw.md "The piano roll"): a MIDI clip's notes in the bottom panel, keys down the left (drum
// names on a channel 10 track), beats across, the velocity lane underneath. The edits themselves are in
// roll_edit.dart.
//
// Mouse: double-click empty space (or click it in draw mode, B) draws a note, dragging on sets its length; a click
// selects (shift adds), a drag on empty space selects a box; drag notes to move them (snapped to the grid, pitches
// row by row), alt-drag copies them, their right edge sets the length, the left edge the start; double-click or
// right-click a note deletes it. Clicking a key plays it. The lane underneath: velocity (drag a stalk, all selected
// notes move together when it is one of them, or across several), a controller of the clip (named by the track's
// synth if it has a MIDI-CI list), or the selected MPE note's own bend, pressure or timbre: click adds a point, drag
// moves it, double-click makes it glide to the next one, right-click removes it, draw mode draws along a drag. Scroll moves through pitches, shift-scroll or a sideways
// swipe through time, cmd-scroll or a pinch zooms time, alt-scroll the rows.
// Keys (while the panel has the mouse's last click): delete, cmd-A, Esc, arrows (shift: octave / bar), cmd-C / V /
// D, Q quantize, B draw mode.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/lanes.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/pattern_editor.dart' show chip, menuChip, stepper;
import 'package:airmidi/desktop/daw/roll_edit.dart';
import 'package:airmidi/shared/play/keys.dart' show Pads;
import 'package:airmidi/shared/synth/controls.dart' show Device;
import 'package:airmidi/shared/ui/theme.dart';

const _laneH = 72.0, _chooseH = 28.0, _barH = 34.0;

List<Note> _clipboard = [];

class PianoRoll extends StatefulWidget {
  final MidiContent content;
  final Track track;
  final History history;
  final VoidCallback changed;
  final double playBeat; // in the content, < 0 when not playing
  final double pasteAt; // where cmd-V puts notes (the start marker inside the clip, else 0)
  final int beatsPerBar;
  final void Function(int pitch, bool on, int velocity) audition;
  final Device? device; // the track's MIDI-CI control list: the lanes' names and readings
  const PianoRoll(
      {super.key,
      required this.content,
      required this.track,
      required this.history,
      required this.changed,
      required this.audition,
      this.playBeat = -1,
      this.pasteAt = 0,
      this.beatsPerBar = 4,
      this.device});

  @override
  State<PianoRoll> createState() => PianoRollState();
}

enum _Drag { none, move, right, left, box, draw }

class PianoRollState extends State<PianoRoll> {
  final selected = <Note>{};
  double grid = 0.25;
  bool drawMode = false, quantizeEnds = false;
  double strength = 1;
  double lastLength = 0.25; // a drawn note's length: the last one drawn or resized

  // the view
  double ppb = 0; // px a beat; 0 until fitted to the clip
  double left = 0; // beat at the left edge
  double rowH = 12;
  double top = 84; // pitch of the top row
  Size _grid = Size.zero;

  // the drag under way
  _Drag _drag = _Drag.none;
  Note? _anchor;
  Map<Note, (double, double, int)> _orig = {};
  double _downBeat = 0;
  int _downPitch = 0;
  Offset _downAt = Offset.zero;
  bool _marked = false, _copy = false;
  Set<Note> _boxBase = {};
  Rect? _box;
  DateTime _lastDown = DateTime(0);
  Offset _lastPos = Offset.zero;
  int? _sounding; // the pitch being auditioned
  Timer? _auditionOff;

  MidiContent get m => widget.content;

  // drum names need more room than note names
  double get _keysW => !widget.track.mpe && widget.track.channel == 9 ? 104 : 64;

  @override
  void didUpdateWidget(PianoRoll oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.content, widget.content)) {
      // undo, or a take rebuilding the clip: the notes are new objects
      selected.removeWhere((n) => !widget.content.notes.contains(n));
    }
  }

  @override
  void dispose() {
    _auditionOff?.cancel();
    if (_sounding != null) widget.audition(_sounding!, false, 0);
    super.dispose();
  }

  void _fit(Size size) {
    final w = size.width - _keysW;
    ppb = w / math.max(1.0, m.length);
    left = 0;
    // centred on the clip's notes (an empty clip: around c4 to c5)
    var lo = m.notes.isEmpty ? 60 : 127, hi = m.notes.isEmpty ? 72 : 0;
    for (final n in m.notes) {
      lo = math.min(lo, n.pitch);
      hi = math.max(hi, n.pitch);
    }
    final rows = (size.height - _laneH - _chooseH - _barH) / rowH;
    top = math.min(127.0, ((lo + hi) / 2 + rows / 2).roundToDouble());
  }

  double x(double beat) => (beat - left) * ppb;
  double beatAt(double px) => left + px / ppb;
  double y(int pitch) => (top - pitch) * rowH;
  int pitchAt(double py) => (top - (py / rowH).floor()).round().clamp(0, 127);

  void _mark() {
    if (_marked) return;
    widget.history.mark();
    _marked = true;
  }

  void _edit(void Function() f) {
    widget.history.mark();
    setState(f);
    widget.changed();
  }

  void _play(int pitch, int vel) {
    if (_sounding != null) widget.audition(_sounding!, false, 0);
    _sounding = pitch;
    widget.audition(pitch, true, vel);
    _auditionOff?.cancel();
    _auditionOff = Timer(const Duration(milliseconds: 250), () {
      if (_sounding != null) widget.audition(_sounding!, false, 0);
      if (mounted) setState(() => _sounding = null);
    });
  }

  (Note, _Drag)? _hit(Offset p) {
    final pitch = pitchAt(p.dy), b = beatAt(p.dx);
    for (final n in m.notes.reversed) {
      if (n.pitch != pitch || b < n.start || b > n.end) continue;
      final l = x(n.start), r = x(n.end);
      final edge = math.min(7.0, (r - l) / 3);
      if (r - p.dx < edge) return (n, _Drag.right);
      if (p.dx - l < edge && r - l > 12) return (n, _Drag.left);
      return (n, _Drag.move);
    }
    return null;
  }

  void _down(PointerDownEvent e) {
    final p = e.localPosition, now = DateTime.now();
    final twice = now.difference(_lastDown).inMilliseconds < 350 && (p - _lastPos).distance < 6;
    _lastDown = twice ? DateTime(0) : now; // a double-click used up: the next click starts again
    _lastPos = p;
    _marked = false;
    _copy = false;
    final hit = _hit(p);
    final shift = HardwareKeyboard.instance.isShiftPressed;
    if (e.buttons == kSecondaryMouseButton || (twice && hit != null)) {
      if (hit != null) {
        _edit(() {
          m.notes.remove(hit.$1);
          selected.remove(hit.$1);
        });
      }
      _drag = _Drag.none;
      return;
    }
    _downAt = p;
    _downBeat = beatAt(p.dx);
    _downPitch = pitchAt(p.dy);
    if (hit != null) {
      final (n, zone) = hit;
      setState(() {
        if (shift) {
          selected.contains(n) ? selected.remove(n) : selected.add(n);
        } else if (!selected.contains(n)) {
          selected
            ..clear()
            ..add(n);
        }
      });
      if (!selected.contains(n)) {
        _drag = _Drag.none;
        return;
      }
      _anchor = n;
      _drag = zone;
      _copy = zone == _Drag.move && HardwareKeyboard.instance.isAltPressed;
      _orig = {for (final s in selected) s: (s.start, s.length, s.pitch)};
      if (zone == _Drag.move) _play(n.pitch, down16to7(n.velocity));
      return;
    }
    if (drawMode || twice) {
      // a new note on the grid, as long as the last one; dragging on sets its length
      final start = math.max(0.0, grid > 0 ? (_downBeat / grid).floor() * grid : _downBeat);
      final n = Note(start, lastLength, _downPitch);
      _edit(() {
        m.notes.add(n);
        selected
          ..clear()
          ..add(n);
      });
      _marked = true;
      _anchor = n;
      _orig = {n: (n.start, n.length, n.pitch)};
      _drag = _Drag.draw;
      _play(n.pitch, 100);
      return;
    }
    _boxBase = shift ? {...selected} : {};
    if (!shift) setState(selected.clear);
    _drag = _Drag.box;
  }

  void _move(PointerMoveEvent e) {
    if (_drag == _Drag.none) return;
    final p = e.localPosition;
    if (_drag == _Drag.box) {
      setState(() {
        _box = Rect.fromPoints(_downAt, p);
        selected
          ..clear()
          ..addAll(_boxBase)
          ..addAll(notesInBox(m.notes, beatAt(_downAt.dx), beatAt(p.dx), pitchAt(_downAt.dy), pitchAt(p.dy)));
      });
      return;
    }
    final a = _anchor!;
    final raw = beatAt(p.dx) - _downBeat;
    if (!_marked && (p - _downAt).distance < 3) return;
    _mark();
    if (_copy) {
      // alt-drag: the originals stay where they were, copies move
      final copies = {for (final n in selected) n: n.copy()};
      m.notes.addAll(copies.values);
      _orig = {for (final e in copies.entries) e.value: _orig[e.key]!};
      _anchor = copies[a];
      selected
        ..clear()
        ..addAll(copies.values);
      _copy = false;
    }
    final an = _anchor!;
    final (aStart, aLen, aPitch) = _orig[an]!;
    for (final e in _orig.entries) {
      e.key
        ..start = e.value.$1
        ..length = e.value.$2
        ..pitch = e.value.$3;
    }
    switch (_drag) {
      case _Drag.move:
        final dt = snapTo(aStart + raw, grid) - aStart, dp = pitchAt(p.dy) - _downPitch;
        moveNotes(selected, dt, dp);
        if (an.pitch != aPitch + _lastDp) _play(an.pitch, down16to7(an.velocity));
        _lastDp = an.pitch - aPitch;
      case _Drag.right || _Drag.draw:
        // a drawn note ends where the pointer is; a resized one moves its end as far as the pointer moved
        final to = _drag == _Drag.draw ? beatAt(p.dx) : aStart + aLen + raw;
        final end = math.max(aStart + (grid > 0 ? grid : 1 / 64), snapTo(to, grid));
        resizeNotes(selected, end - (aStart + aLen));
        lastLength = an.length;
      case _Drag.left:
        trimStarts(selected, snapTo(aStart + raw, grid) - aStart);
      default:
    }
    setState(() {});
    widget.changed();
  }

  int _lastDp = 0;

  void _up(PointerEvent e) {
    if (_drag == _Drag.move && !_marked && !HardwareKeyboard.instance.isShiftPressed && _anchor != null) {
      // a click on one of several selected notes: just that one
      setState(() => selected
        ..clear()
        ..add(_anchor!));
    }
    _drag = _Drag.none;
    _box = null;
    _lastDp = 0;
    m.notes.sort((a, b) => a.start.compareTo(b.start));
    setState(() {});
  }

  void _signal(PointerSignalEvent e) {
    if (e is! PointerScrollEvent) return;
    final k = HardwareKeyboard.instance;
    setState(() {
      if (k.isMetaPressed || k.isControlPressed) {
        _zoomTime(math.exp(-e.scrollDelta.dy / 300), e.localPosition.dx);
      } else if (k.isAltPressed) {
        rowH = (rowH * math.exp(-e.scrollDelta.dy / 300)).clamp(5.0, 30.0);
      } else {
        left = math.max(0, left + e.scrollDelta.dx / ppb);
        top = (top - e.scrollDelta.dy / rowH).clamp(_grid.height / rowH, 127.0);
      }
    });
  }

  void _zoomTime(double f, double around) {
    final b = beatAt(around);
    ppb = (ppb * f).clamp(8.0, 800.0);
    left = math.max(0, b - around / ppb);
  }

  double _lastScale = 1;

  // ---- keys (the page hands them over while the panel has the last click)

  /// True when the key was the piano roll's.
  bool key(KeyEvent e) {
    if (e is! KeyDownEvent && e is! KeyRepeatEvent) return false;
    final k = e.logicalKey, keys = HardwareKeyboard.instance;
    final cmd = keys.isMetaPressed || keys.isControlPressed, shift = keys.isShiftPressed;
    final step = grid > 0 ? grid : 0.25;
    if (k == LogicalKeyboardKey.backspace || k == LogicalKeyboardKey.delete) {
      if (selected.isNotEmpty) {
        _edit(() {
          m.notes.removeWhere(selected.contains);
          selected.clear();
        });
      }
      return true; // never the clip's delete while the roll has the keys
    }
    if (cmd && k == LogicalKeyboardKey.keyA) {
      setState(() => selected.addAll(m.notes));
    } else if (k == LogicalKeyboardKey.escape) {
      setState(selected.clear);
    } else if (selected.isNotEmpty && (k == LogicalKeyboardKey.arrowUp || k == LogicalKeyboardKey.arrowDown)) {
      final d = (k == LogicalKeyboardKey.arrowUp ? 1 : -1) * (shift ? 12 : 1);
      _edit(() => transpose(selected, d));
      final n = selected.first;
      _play(n.pitch, down16to7(n.velocity));
    } else if (selected.isNotEmpty && (k == LogicalKeyboardKey.arrowLeft || k == LogicalKeyboardKey.arrowRight)) {
      final d = (k == LogicalKeyboardKey.arrowRight ? 1 : -1) * (shift ? widget.beatsPerBar.toDouble() : step);
      _edit(() => moveNotes(selected, d, 0));
    } else if (cmd && k == LogicalKeyboardKey.keyC) {
      _clipboard = [for (final n in selected) n.copy()];
    } else if (cmd && k == LogicalKeyboardKey.keyV) {
      final notes = pasted(_clipboard, widget.pasteAt);
      if (notes.isNotEmpty) {
        _edit(() {
          m.notes.addAll(notes);
          selected
            ..clear()
            ..addAll(notes);
        });
      }
    } else if (cmd && k == LogicalKeyboardKey.keyD) {
      final notes = duplicated(selected.toList(), grid);
      if (notes.isNotEmpty) {
        _edit(() {
          m.notes.addAll(notes);
          selected
            ..clear()
            ..addAll(notes);
        });
      }
    } else if (!cmd && k == LogicalKeyboardKey.keyQ) {
      _quantize();
    } else if (!cmd && k == LogicalKeyboardKey.keyB) {
      setState(() => drawMode = !drawMode);
    } else {
      return false;
    }
    return true;
  }

  Iterable<Note> get _targets => selected.isEmpty ? m.notes : selected;

  void _quantize() => _edit(() => quantize(_targets, grid, strength: strength, ends: quantizeEnds));

  // ---- build

  @override
  Widget build(BuildContext context) => LayoutBuilder(builder: (context, box) {
        if (ppb == 0) _fit(box.biggest);
        final drums = !widget.track.mpe && widget.track.channel == 9;
        return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          SizedBox(height: _barH, child: _toolbar()),
          Expanded(
            child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              SizedBox(
                width: _keysW,
                child: Listener(
                  onPointerDown: (e) => _keyDown(pitchAt(e.localPosition.dy)),
                  onPointerUp: (_) => _keyUp(),
                  onPointerCancel: (_) => _keyUp(),
                  onPointerSignal: _signal,
                  child: CustomPaint(
                      size: Size.infinite, painter: _KeysPainter(this, drums, _sounding, widget.track.channel)),
                ),
              ),
              Expanded(
                child: LayoutBuilder(builder: (context, b) {
                  _grid = b.biggest;
                  return Listener(
                    behavior: HitTestBehavior.opaque,
                    onPointerDown: _down,
                    onPointerMove: _move,
                    onPointerUp: _up,
                    onPointerCancel: _up,
                    onPointerSignal: _signal,
                    onPointerPanZoomStart: (_) => _lastScale = 1,
                    onPointerPanZoomUpdate: (e) => setState(() {
                      if ((e.scale - _lastScale).abs() > 1e-4) {
                        _zoomTime(e.scale / _lastScale, e.localPosition.dx);
                        _lastScale = e.scale;
                      } else {
                        left = math.max(0, left - e.panDelta.dx / ppb);
                        top = (top + e.panDelta.dy / rowH).clamp(_grid.height / rowH, 127.0);
                      }
                    }),
                    child: MouseRegion(
                      cursor: drawMode ? SystemMouseCursors.precise : SystemMouseCursors.basic,
                      child: CustomPaint(size: Size.infinite, painter: _GridPainter(this)),
                    ),
                  );
                }),
              ),
            ]),
          ),
          SizedBox(height: _chooseH, child: _chooser()),
          SizedBox(
            height: _laneH,
            child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              SizedBox(width: _keysW),
              Expanded(
                child: Listener(
                  key: const ValueKey('lane'),
                  behavior: HitTestBehavior.opaque,
                  onPointerDown: (e) => lane == 'velocity' ? _laneDown(e) : _ptDown(e),
                  onPointerMove: (e) => lane == 'velocity' ? _lane(e.localPosition) : _ptMove(e),
                  onPointerUp: (_) => lane == 'velocity' ? _laneUp() : _ptUp(),
                  child: CustomPaint(size: Size.infinite, painter: _LanePainter(this)),
                ),
              ),
            ]),
          ),
        ]);
      });

  void _keyDown(int pitch) {
    _auditionOff?.cancel();
    if (_sounding != null) widget.audition(_sounding!, false, 0);
    setState(() => _sounding = pitch);
    widget.audition(pitch, true, 100);
  }

  void _keyUp() {
    if (_sounding != null) widget.audition(_sounding!, false, 0);
    setState(() => _sounding = null);
  }

  // ---- velocity lane

  Map<Note, int> _velOrig = {};
  Note? _velNote;

  Note? _stalkAt(double px) {
    Note? best;
    var bestD = 8.0;
    for (final n in m.notes) {
      final d = (x(n.start) - px).abs();
      if (d < bestD || (d == bestD && selected.contains(n))) {
        best = n;
        bestD = d;
      }
    }
    return best;
  }

  void _laneDown(PointerDownEvent e) {
    _marked = false;
    _velNote = _stalkAt(e.localPosition.dx);
    _velOrig = {for (final n in m.notes) n: down16to7(n.velocity)};
    _lane(e.localPosition);
  }

  void _lane(Offset p) {
    final hit = _velNote != null && selected.contains(_velNote) ? _velNote : _stalkAt(p.dx);
    if (hit == null) return;
    final v = (1 + (1 - p.dy / _laneH) * 126).round().clamp(1, 127);
    _mark();
    setState(() {
      if (selected.contains(hit) && identical(hit, _velNote)) {
        // one of the selection: all of it moves by as much
        final d = v - _velOrig[hit]!;
        for (final n in selected) {
          n.velocity = up7to16((_velOrig[n]! + d).clamp(1, 127));
        }
      } else {
        hit.velocity = up7to16(v);
      }
    });
    widget.changed();
  }

  void _laneUp() => _velNote = null;

  // ---- controller lanes and a note's own expression

  /// The lane shown under the roll: 'velocity', a lane of the clip ('cc74', 'bend', ...), or the selected MPE note's
  /// own 'note:bend', 'note:pressure', 'note:timbre'.
  String lane = 'velocity';

  /// The points the lane edits, where their time 0 is, and how far they reach (a note's own: its length).
  (List<Point>, double, double)? get lanePoints {
    if (lane == 'velocity') return null;
    if (lane.startsWith('note:')) {
      if (selected.length != 1) return null;
      final n = selected.first;
      return (n.expression.putIfAbsent(lane.substring(5), () => []), n.start, n.length);
    }
    return (m.lanes.putIfAbsent(lane, () => []), 0.0, m.length);
  }

  /// Selects [notes] (for tests and the page).
  void setSelected(Set<Note> notes) => setState(() => selected
    ..clear()
    ..addAll(notes));

  /// Shows lane [key] (made empty if the clip has none).
  void showLane(String key) => setState(() {
        if (!key.startsWith('note:')) m.lanes.putIfAbsent(key, () => []);
        lane = key;
      });

  int? _pt; // the point being dragged
  bool _drawing = false;
  DateTime _ptLast = DateTime(0);

  double _ptTime(double px, double base, double len) => snapTo(beatAt(px) - base, grid).clamp(0.0, len);
  double ptY(int v) => (1 - laneFraction(lane, v)) * (_laneH - 8) + 4;
  int _ptValue(double py) => laneValueAt(lane, 1 - (py - 4) / (_laneH - 8));

  int? _pointAt(List<Point> pts, double base, Offset p) {
    int? best;
    var bestD = 8.0;
    for (var i = 0; i < pts.length; i++) {
      final d = (Offset(x(base + pts[i].t), ptY(pts[i].v)) - p).distance;
      if (d < bestD) {
        best = i;
        bestD = d;
      }
    }
    return best;
  }

  void _sortPts(List<Point> pts) {
    final idx = {for (var i = 0; i < pts.length; i++) pts[i]: i};
    pts.sort((a, b) => a.t != b.t ? a.t.compareTo(b.t) : idx[a]!.compareTo(idx[b]!));
  }

  void _ptDown(PointerDownEvent e) {
    final lp = lanePoints;
    if (lp == null) return;
    final (pts, base, len) = lp;
    final p = e.localPosition, now = DateTime.now();
    final twice = now.difference(_ptLast).inMilliseconds < 350;
    _ptLast = twice ? DateTime(0) : now;
    _marked = false;
    final i = _pointAt(pts, base, p);
    if (e.buttons == kSecondaryMouseButton) {
      if (i != null) _edit(() => pts.removeAt(i));
      return;
    }
    if (i != null && twice) {
      // a double-click on a point: glide from it to the next one, or hold again
      _edit(() => pts[i] = Point(pts[i].t, pts[i].v, ramp: !pts[i].ramp));
      return;
    }
    if (i != null) {
      _pt = i;
      return;
    }
    final pt = Point(_ptTime(p.dx, base, len), _ptValue(p.dy));
    _mark();
    setState(() {
      pts.add(pt);
      _sortPts(pts);
      _pt = pts.indexOf(pt);
    });
    _drawing = drawMode;
    widget.changed();
  }

  void _ptMove(PointerMoveEvent e) {
    final lp = lanePoints, i = _pt;
    if (lp == null || i == null || i >= lp.$1.length) return;
    final (pts, base, len) = lp;
    final p = e.localPosition, t = _ptTime(p.dx, base, len), v = _ptValue(p.dy);
    _mark();
    setState(() {
      final np = Point(t, v, ramp: _drawing ? false : pts[i].ramp);
      if (_drawing) {
        // draw mode: one point a grid step along the drag, replacing what was there
        final half = grid > 0 ? grid / 2 : 1 / 128;
        pts.removeWhere((q) => (q.t - t).abs() < half);
        pts.add(np);
      } else {
        pts[i] = np;
      }
      _sortPts(pts);
      _pt = pts.indexOf(np);
    });
    widget.changed();
  }

  void _ptUp() {
    _pt = null;
    _drawing = false;
    setState(() {});
  }

  Widget _chooser() {
    final noteLanes = widget.track.mpe && selected.length == 1;
    if (lane.startsWith('note:') && !noteLanes) lane = 'velocity';
    if (!lane.startsWith('note:') && lane != 'velocity' && !m.lanes.containsKey(lane)) lane = 'velocity';
    final d = widget.device;
    return Row(children: [
      SizedBox(
        width: _keysW,
        child: Text(laneTitle(lane == 'velocity' ? 'velocity' : lane, d),
            style: const TextStyle(color: cGrey, fontSize: 10), overflow: TextOverflow.ellipsis),
      ),
      Expanded(
        child: SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          child: Row(children: [
            chip('velocity', lane == 'velocity', () => setState(() => lane = 'velocity')),
            for (final k in m.lanes.keys)
              Padding(
                padding: const EdgeInsets.only(left: 3),
                child: GestureDetector(
                  onSecondaryTapDown: (t) => _laneMenu(k, t.globalPosition),
                  child: chip(laneTitle(k, d), lane == k, () => setState(() => lane = k)),
                ),
              ),
            if (noteLanes)
              for (final k in const ['note:bend', 'note:pressure', 'note:timbre'])
                Padding(
                    padding: const EdgeInsets.only(left: 3),
                    child: chip(laneTitle(k, d), lane == k, () => setState(() => lane = k))),
            const SizedBox(width: 6),
            menuChip('+ lane ▾', _addLane),
            const SizedBox(width: 10),
            Text(
                lane == 'velocity'
                    ? ''
                    : 'click: a point · drag: move it · double-click: glide to the next · right-click: remove'
                        '${drawMode ? ' · draw mode: drag to draw' : ''}',
                style: const TextStyle(color: cGrey, fontSize: 11)),
          ]),
        ),
      ),
    ]);
  }

  Future<T?> _menu<T>(Offset at, List<(String, T)> items) => showMenu<T>(
        context: context,
        color: cBg,
        position: RelativeRect.fromLTRB(at.dx, at.dy, at.dx + 1, at.dy + 1),
        items: [
          for (final (label, v) in items)
            PopupMenuItem(value: v, height: 30, child: Text(label, style: const TextStyle(color: cN, fontSize: 13))),
        ],
      );

  Future<void> _addLane(Offset at) async {
    final have = m.lanes.keys.toSet();
    final k = await _menu<String>(at, [
      for (final (title, key) in laneChoices(widget.device))
        if (!have.contains(key)) (title, key),
      ('another CC ...', 'other'),
    ]);
    if (k == null) return;
    var key = k;
    if (k == 'other') {
      final n = await _askCc();
      if (n == null) return;
      key = 'cc$n';
    }
    _edit(() {
      m.lanes.putIfAbsent(key, () => []);
      lane = key;
    });
  }

  Future<int?> _askCc() async {
    final ctl = TextEditingController();
    final v = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        backgroundColor: cBg,
        title: const Text('CC number (0-127)', style: TextStyle(color: cN, fontSize: 15)),
        content: TextField(controller: ctl, autofocus: true, onSubmitted: (v) => Navigator.of(context).pop(v)),
        actions: [TextButton(onPressed: () => Navigator.of(context).pop(ctl.text), child: const Text('ok'))],
      ),
    );
    final n = int.tryParse(v ?? '');
    return n == null || n < 0 || n > 127 ? null : n;
  }

  Future<void> _laneMenu(String key, Offset at) async {
    final v = await _menu<String>(at, [('remove the lane and its points', 'remove'), ('clear its points', 'clear')]);
    if (v == 'remove') {
      _edit(() {
        m.lanes.remove(key);
        if (lane == key) lane = 'velocity';
      });
    }
    if (v == 'clear') _edit(() => m.lanes[key]?.clear());
  }

  // ---- the bar above the roll

  Widget _toolbar() {
    final sel = selected.length;
    return SingleChildScrollView(
      scrollDirection: Axis.horizontal,
      child: Row(children: [
        const Text('grid ', style: TextStyle(color: cGrey, fontSize: 12)),
        for (final (name, g) in kRollGrids)
          Padding(
              padding: const EdgeInsets.only(right: 3), child: chip(name, grid == g, () => setState(() => grid = g))),
        const SizedBox(width: 10),
        chip('draw (B)', drawMode, () => setState(() => drawMode = !drawMode)),
        const SizedBox(width: 10),
        chip('quantize (Q)', false, _quantize),
        stepper('', '${(strength * 100).round()}%', () => setState(() => strength = (strength - 0.25).clamp(0.25, 1)),
            () => setState(() => strength = (strength + 0.25).clamp(0.25, 1))),
        chip('ends', quantizeEnds, () => setState(() => quantizeEnds = !quantizeEnds)),
        const SizedBox(width: 10),
        chip('legato', false, () => _edit(() => legato(_targets))),
        const SizedBox(width: 6),
        stepper('transpose', '', () => _edit(() => transpose(_targets, -1)), () => _edit(() => transpose(_targets, 1))),
        stepper('octave', '', () => _edit(() => transpose(_targets, -12)), () => _edit(() => transpose(_targets, 12))),
        stepper('velocity', '', () => _edit(() => nudgeVelocity(_targets, -8)),
            () => _edit(() => nudgeVelocity(_targets, 8))),
        const SizedBox(width: 14),
        Text(sel == 0 ? '${m.notes.length} notes (edits apply to all)' : '$sel selected',
            style: const TextStyle(color: cGrey, fontSize: 12)),
      ]),
    );
  }
}

bool _black(int n) => const [1, 3, 6, 8, 10].contains(n % 12);

void _label(Canvas c, String t, double right, double cy, double size, Color color) {
  final tp = TextPainter(
      text: TextSpan(text: t, style: TextStyle(color: color, fontSize: size)), textDirection: TextDirection.ltr)
    ..layout();
  tp.paint(c, Offset(right - tp.width, cy - tp.height / 2));
}

class _KeysPainter extends CustomPainter {
  final PianoRollState s;
  final bool drums;
  final int? sounding;
  final int channel;
  _KeysPainter(this.s, this.drums, this.sounding, this.channel);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    final lo = s.pitchAt(size.height), hi = s.pitchAt(0);
    for (var n = lo; n <= hi; n++) {
      final y = s.y(n), black = _black(n);
      // white keys full width, black ones shorter over them, as on a keyboard
      c.drawRect(Rect.fromLTWH(0, y, size.width - 2, s.rowH), Paint()..color = const Color(0xFFB9C2C7));
      if (black) {
        c.drawRect(Rect.fromLTWH(0, y, (size.width - 2) * 0.6, s.rowH), Paint()..color = const Color(0xFF1C2329));
      }
      if (n % 12 == 0 || n % 12 == 5) {
        c.drawLine(Offset(0, y + s.rowH), Offset(size.width - 2, y + s.rowH), Paint()..color = const Color(0xFF6B757B));
      }
      if (n == sounding) {
        c.drawRect(Rect.fromLTWH(0, y, size.width - 2, s.rowH), Paint()..color = cAc.withValues(alpha: 0.85));
      }
      final name = drums ? Pads.drum(n) : (n % 12 == 0 ? pitchName(n) : '');
      if (name.isNotEmpty && s.rowH >= 8) {
        _label(c, name, size.width - 5, y + s.rowH / 2, math.min(10, s.rowH - 1), cBg);
      }
    }
  }

  @override
  bool shouldRepaint(_KeysPainter old) => true;
}

class _GridPainter extends CustomPainter {
  final PianoRollState s;
  _GridPainter(this.s);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    final m = s.m;
    final lo = s.pitchAt(size.height), hi = s.pitchAt(0);
    for (var n = lo; n <= hi; n++) {
      c.drawRect(Rect.fromLTWH(0, s.y(n), size.width, s.rowH),
          Paint()..color = _black(n) ? const Color(0xFF0D1317) : const Color(0xFF131B20));
      if (n % 12 == 0) {
        c.drawLine(Offset(0, s.y(n) + s.rowH), Offset(size.width, s.y(n) + s.rowH),
            Paint()..color = cGrey.withValues(alpha: 0.35));
      }
    }
    // the grid, then beats and bars over it
    final b0 = s.left.floor(), b1 = s.beatAt(size.width).ceil();
    if (s.grid > 0 && s.grid * s.ppb >= 6) {
      for (var g = (s.left / s.grid).floor(); g * s.grid <= b1; g++) {
        final px = s.x(g * s.grid);
        c.drawLine(Offset(px, 0), Offset(px, size.height), Paint()..color = cGrey.withValues(alpha: 0.07));
      }
    }
    for (var b = b0; b <= b1; b++) {
      final px = s.x(b.toDouble());
      c.drawLine(Offset(px, 0), Offset(px, size.height),
          Paint()..color = cGrey.withValues(alpha: b % s.widget.beatsPerBar == 0 ? 0.5 : 0.2));
    }
    // past the content's end nothing plays
    c.drawRect(Rect.fromLTRB(s.x(m.length), 0, size.width, size.height), Paint()..color = cBg.withValues(alpha: 0.6));
    for (final n in m.notes) {
      final x0 = s.x(n.start), x1 = math.max(x0 + 3, s.x(n.end)), y = s.y(n.pitch);
      if (x1 < 0 || x0 > size.width || y + s.rowH < 0 || y > size.height) continue;
      final sel = s.selected.contains(n);
      final a = 0.45 + 0.55 * down16to7(n.velocity) / 127;
      final r = Rect.fromLTRB(x0, y + 1, x1, y + s.rowH - 1);
      c.drawRect(r, Paint()..color = sel ? cAc : stripHues[0].drag.withValues(alpha: a));
      c.drawRect(
          r,
          Paint()
            ..color = cBg.withValues(alpha: 0.6)
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1);
      final bend = n.expression['bend'];
      if (bend != null && bend.length > 1) {
        // the note's own bend along it (half the row = a full bend)
        final path = Path();
        for (var i = 0; i < bend.length; i++) {
          final px = s.x(n.start + bend[i].t), py = y + s.rowH / 2 - (bend[i].v / 0x100000000 - 0.5) * s.rowH * 4;
          i == 0 ? path.moveTo(px, py) : path.lineTo(px, py);
        }
        c.drawPath(
            path,
            Paint()
              ..color = sel ? cBg : cAc
              ..style = PaintingStyle.stroke
              ..strokeWidth = 1.2);
      }
    }
    final box = s._box;
    if (box != null) {
      c.drawRect(box, Paint()..color = cAc.withValues(alpha: 0.12));
      c.drawRect(
          box,
          Paint()
            ..color = cAc
            ..style = PaintingStyle.stroke);
    }
    final play = s.widget.playBeat;
    if (play >= 0) {
      c.drawLine(
          Offset(s.x(play), 0),
          Offset(s.x(play), size.height),
          Paint()
            ..color = cAc
            ..strokeWidth = 2);
    }
  }

  @override
  bool shouldRepaint(_GridPainter old) => true;
}

class _LanePainter extends CustomPainter {
  final PianoRollState s;
  _LanePainter(this.s);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    c.drawRect(Offset.zero & size, Paint()..color = const Color(0xFF10171C));
    final lp = s.lanePoints;
    if (lp == null) {
      for (final n in s.m.notes) {
        final px = s.x(n.start);
        if (px < -4 || px > size.width + 4) continue;
        final v = down16to7(n.velocity) / 127, top = size.height - v * (size.height - 6);
        final col = s.selected.contains(n) ? cAc : stripHues[0].drag;
        c.drawLine(
            Offset(px, size.height),
            Offset(px, top),
            Paint()
              ..color = col
              ..strokeWidth = 2);
        c.drawCircle(Offset(px, top), 3, Paint()..color = col);
      }
      return;
    }
    final (pts, base, len) = lp;
    // outside what the points can reach (past the clip's end, or outside the note): darker
    final x0 = s.x(base), x1 = s.x(base + len);
    c.drawRect(Rect.fromLTRB(0, 0, x0, size.height), Paint()..color = cBg.withValues(alpha: 0.7));
    c.drawRect(Rect.fromLTRB(x1, 0, size.width, size.height), Paint()..color = cBg.withValues(alpha: 0.7));
    if (laneCentred(s.lane)) {
      final cy = s.ptY(kCentre32);
      c.drawLine(Offset(0, cy), Offset(size.width, cy), Paint()..color = cGrey.withValues(alpha: 0.4));
    }
    if (pts.isEmpty) return;
    final line = Paint()
      ..color = stripHues[1].drag
      ..strokeWidth = 1.6
      ..style = PaintingStyle.stroke;
    final path = Path()..moveTo(s.x(base + pts.first.t), s.ptY(pts.first.v));
    for (var i = 0; i < pts.length; i++) {
      final p = pts[i], px = s.x(base + p.t), py = s.ptY(p.v);
      final next = i + 1 < pts.length ? pts[i + 1] : null;
      final nx = next == null ? x1 : s.x(base + next.t);
      path.lineTo(px, py);
      if (next != null && p.ramp) {
        path.lineTo(nx, s.ptY(next.v));
      } else {
        path.lineTo(nx, py); // holds until the next point
      }
    }
    c.drawPath(path, line);
    for (var i = 0; i < pts.length; i++) {
      final p = pts[i], at = Offset(s.x(base + p.t), s.ptY(p.v)), on = i == s._pt;
      c.drawCircle(at, on ? 5 : 3.5, Paint()..color = on ? cAc : stripHues[1].drag);
      if (on) {
        final tp = TextPainter(
            text: TextSpan(
                text: laneDisplay(s.lane, p.v, s.widget.device), style: const TextStyle(color: cN, fontSize: 11)),
            textDirection: TextDirection.ltr)
          ..layout();
        tp.paint(c, Offset(at.dx + 8, (at.dy - 16).clamp(0.0, size.height - 14)));
      }
    }
  }

  @override
  bool shouldRepaint(_LanePainter old) => true;
}

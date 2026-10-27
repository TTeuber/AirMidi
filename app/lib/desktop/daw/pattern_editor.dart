// The sequencer (docs/daw.md "The sequencer"): a pattern clip's rows by steps in the bottom panel, and under the grid
// a lane for one setting of every step in the selected row: velocity, gate, probability, nudge or repeat.
//
// Grid: a click on an empty cell adds a step (and plays it), dragging on from there paints more; a click on a step
// removes it, dragging it up or down sets its velocity (the fill's height), dragging it sideways erases along. Row labels: a click selects the row for
// the lane, M mutes it, a right-click opens rename, note up/down, move, remove. Long patterns show 16 steps a page
// (and follow the playhead while playing, if "follow" is on). The lane: drag a bar, or sweep across; double-click
// puts a step's setting back to its default.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart' hide Step;

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/shared/play/keys.dart' show Pads;
import 'package:airmidi/shared/ui/theme.dart';

enum StepParam { velocity, gate, probability, nudge, repeat }

const _labelW = 132.0, _laneH = 72.0, _minCell = 22.0;

class PatternEditor extends StatefulWidget {
  final Pattern pattern;
  final Track track;
  final History history;
  final VoidCallback changed;
  final int playStep; // the step playing now, -1 when this clip is not playing
  final void Function(int pitch, bool on, int velocity)? audition;
  const PatternEditor(
      {super.key,
      required this.pattern,
      required this.track,
      required this.history,
      required this.changed,
      this.playStep = -1,
      this.audition});

  @override
  State<PatternEditor> createState() => PatternEditorState();
}

enum _Mode { none, paint, erase, velocity }

class PatternEditorState extends State<PatternEditor> {
  int row = 0, page = 0;
  StepParam param = StepParam.velocity;
  bool follow = true;
  int topRow = 0; // rows scrolled off the top

  _Mode _mode = _Mode.none;
  int _r = 0, _s = 0;
  double _downY = 0;
  int _origVel = 0;
  bool _marked = false, _moved = false;
  DateTime _lastLane = DateTime(0);
  int _lastLaneStep = -1;
  Timer? _off;
  int? _sounding;

  Pattern get p => widget.pattern;

  @override
  void dispose() {
    _off?.cancel();
    if (_sounding != null) widget.audition?.call(_sounding!, false, 0);
    super.dispose();
  }

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
    final a = widget.audition;
    if (a == null) return;
    if (_sounding != null) a(_sounding!, false, 0);
    _sounding = pitch;
    a(pitch, true, vel);
    _off?.cancel();
    _off = Timer(const Duration(milliseconds: 200), () {
      if (_sounding != null) a(_sounding!, false, 0);
      _sounding = null;
    });
  }

  // ---- geometry

  bool get paged => _cellsW / p.steps < _minCell && p.steps > 16;
  int get pages => paged ? (p.steps / 16).ceil() : 1;
  int get first => paged ? page * 16 : 0;
  int get shown => paged ? math.min(16, p.steps - first) : p.steps;
  double _cellsW = 600, _rowH = 22;

  double get rowH => _rowH;
  double cellX(int step) => _labelW + (step - first) * (_cellsW / shown);
  double get cellW => _cellsW / shown;
  int? stepAt(double x) {
    if (x < _labelW) return null;
    final s = first + ((x - _labelW) / cellW).floor();
    return s >= first && s < first + shown ? s : null;
  }

  int? rowAt(double y) {
    final r = topRow + (y / _rowH).floor();
    return r >= 0 && r < p.rows.length ? r : null;
  }

  // ---- the grid

  void _gridDown(PointerDownEvent e) {
    final pos = e.localPosition, r = rowAt(pos.dy);
    _marked = false;
    _moved = false;
    _mode = _Mode.none;
    if (r == null) return;
    if (pos.dx < _labelW) {
      if (e.buttons == kSecondaryMouseButton) {
        _rowMenu(r, e.position);
      } else if (pos.dx > _labelW - 24) {
        _edit(() => p.rows[r].mute = !p.rows[r].mute);
      } else {
        setState(() => row = r);
        _play(p.rows[r].note, 100);
      }
      return;
    }
    final s = stepAt(pos.dx);
    if (s == null) return;
    setState(() => row = r);
    _r = r;
    _s = s;
    _downY = pos.dy;
    final st = p.cells[r][s];
    if (st == null) {
      _mark();
      setState(() => p.cells[r][s] = Step());
      _play(p.rows[r].note, 100);
      widget.changed();
      _mode = _Mode.paint;
    } else {
      _origVel = down16to7(st.velocity);
      _mode = _Mode.velocity;
    }
  }

  void _gridMove(PointerMoveEvent e) {
    final pos = e.localPosition;
    switch (_mode) {
      case _Mode.paint:
        // every cell between the last one painted and the pointer: a fast drag skips cells between its events
        final r = rowAt(pos.dy), s = stepAt(pos.dx);
        if (r == null || s == null) return;
        if (r != _r) _s = s;
        setState(() {
          for (var x = math.min(_s, s); x <= math.max(_s, s); x++) {
            p.cells[r][x] ??= Step();
          }
        });
        _r = r;
        _s = s;
        widget.changed();
      case _Mode.erase:
        final r = rowAt(pos.dy), s = stepAt(pos.dx);
        if (r == null || s == null) return;
        if (r != _r) _s = s;
        setState(() {
          for (var x = math.min(_s, s); x <= math.max(_s, s); x++) {
            p.cells[r][x] = null;
          }
        });
        _r = r;
        _s = s;
        widget.changed();
      case _Mode.velocity:
        final dy = _downY - pos.dy;
        if (!_moved && stepAt(pos.dx) != _s && stepAt(pos.dx) != null) {
          // sideways off the step it started on: erase along the row instead
          _mark();
          _mode = _Mode.erase;
          setState(() => p.cells[_r][_s] = null);
          _gridMove(e);
          return;
        }
        if (!_moved && dy.abs() < 3) return;
        _moved = true;
        _mark();
        final v = (_origVel + dy * 127 / (_rowH * 4)).round().clamp(1, 127);
        setState(() => p.cells[_r][_s]!.velocity = up7to16(v));
        widget.changed();
      default:
    }
  }

  void _gridUp(PointerEvent e) {
    if (_mode == _Mode.velocity && !_moved) _edit(() => p.cells[_r][_s] = null); // a click on a step: off
    _mode = _Mode.none;
  }

  void _scroll(PointerSignalEvent e) {
    if (e is! PointerScrollEvent) return;
    final visible = (context.size?.height ?? 300) ~/ _rowH;
    setState(() => topRow = (topRow + e.scrollDelta.dy.sign.toInt()).clamp(0, math.max(0, p.rows.length - visible)));
  }

  Future<void> _rowMenu(int r, Offset at) async {
    final pr = p.rows[r];
    final v = await _menu(at, [
      ('rename', 'rename'),
      ('note up (${pitchName(math.min(127, pr.note + 1))})', 'up'),
      ('note down (${pitchName(math.max(0, pr.note - 1))})', 'down'),
      (pr.mute ? 'unmute' : 'mute', 'mute'),
      if (r > 0) ('move up', 'mup'),
      if (r < p.rows.length - 1) ('move down', 'mdown'),
      if (p.rows.length > 1) ('remove', 'remove'),
    ]);
    switch (v) {
      case 'rename':
        final name = await _ask('row name', pr.name);
        if (name != null) _edit(() => pr.name = name);
      case 'up' || 'down':
        _edit(() {
          pr.note = (pr.note + (v == 'up' ? 1 : -1)).clamp(0, 127);
          final drum = Pads.drum(pr.note);
          if (RegExp(r'^[a-g]#?-?\d$').hasMatch(pr.name) || Pads.drum(pr.note + (v == 'up' ? -1 : 1)) == pr.name) {
            pr.name = drum.isNotEmpty && widget.track.channel == 9 ? drum : pitchName(pr.note);
          }
        });
        _play(pr.note, 100);
      case 'mute':
        _edit(() => pr.mute = !pr.mute);
      case 'mup' || 'mdown':
        final to = v == 'mup' ? r - 1 : r + 1;
        _edit(() {
          p.rows.insert(to, p.rows.removeAt(r));
          p.cells.insert(to, p.cells.removeAt(r));
          row = to;
        });
      case 'remove':
        _edit(() {
          p.removeRow(r);
          row = row.clamp(0, p.rows.length - 1);
        });
    }
  }

  Future<T?> _menu<T>(Offset at, List<(String, T)> items) => showMenu<T>(
        context: context,
        color: cBg,
        position: RelativeRect.fromLTRB(at.dx, at.dy, at.dx + 1, at.dy + 1),
        items: [
          for (final (label, v) in items)
            PopupMenuItem(value: v, height: 32, child: Text(label, style: const TextStyle(color: cN, fontSize: 13))),
        ],
      );

  Future<String?> _ask(String title, String now) async {
    final ctl = TextEditingController(text: now);
    final v = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        backgroundColor: cBg,
        title: Text(title, style: const TextStyle(color: cN, fontSize: 15)),
        content: TextField(controller: ctl, autofocus: true, onSubmitted: (v) => Navigator.of(context).pop(v)),
        actions: [TextButton(onPressed: () => Navigator.of(context).pop(ctl.text), child: const Text('ok'))],
      ),
    );
    return v == null || v.trim().isEmpty ? null : v.trim();
  }

  // ---- the lane

  (double, double) _range(StepParam q) => switch (q) {
        StepParam.velocity => (1, 127),
        StepParam.gate => (0.05, 2),
        StepParam.probability => (0, 1),
        StepParam.nudge => (-0.5, 0.5),
        StepParam.repeat => (1, 4),
      };

  double value(Step st, StepParam q) => switch (q) {
        StepParam.velocity => down16to7(st.velocity).toDouble(),
        StepParam.gate => st.gate,
        StepParam.probability => st.probability,
        StepParam.nudge => st.nudge,
        StepParam.repeat => st.repeat.toDouble(),
      };

  void _set(Step st, StepParam q, double v) {
    switch (q) {
      case StepParam.velocity:
        st.velocity = up7to16(v.round().clamp(1, 127));
      case StepParam.gate:
        st.gate = (v * 20).round() / 20;
      case StepParam.probability:
        st.probability = (v * 20).round() / 20;
      case StepParam.nudge:
        st.nudge = (v * 20).round() / 20;
      case StepParam.repeat:
        st.repeat = v.round().clamp(1, 4);
    }
  }

  void _laneAt(Offset pos, {bool down = false}) {
    final s = stepAt(pos.dx);
    if (s == null || row >= p.rows.length) return;
    final st = p.cells[row][s];
    if (st == null) return;
    final now = DateTime.now();
    if (down && s == _lastLaneStep && now.difference(_lastLane).inMilliseconds < 350) {
      // a double-click: back to the default
      final d = Step();
      _edit(() => _set(st, param, value(d, param)));
      _lastLane = DateTime(0);
      return;
    }
    if (down) {
      _lastLane = now;
      _lastLaneStep = s;
    }
    final (lo, hi) = _range(param);
    final f = (1 - pos.dy / _laneH).clamp(0.0, 1.0);
    _mark();
    setState(() => _set(st, param, lo + f * (hi - lo)));
    widget.changed();
  }

  // ---- the bar

  Future<void> _rowsMenu(Offset at) async {
    final v = await _menu(at, [
      ('drums: GM, 6 rows', 'gm6'),
      ('drums: GM, 12 rows', 'gm12'),
      ('a scale or octave ...', 'scale'),
    ]);
    if (v == 'gm6') _edit(() => _rows(gmDrumRows()));
    if (v == 'gm12') _edit(() => _rows(gmKitRows()));
    if (v == 'scale') {
      final rows = await _scaleDialog();
      if (rows != null) _edit(() => _rows(rows));
    }
  }

  Future<List<PatternRow>?> _scaleDialog() {
    var key = 0, octave = 3, octaves = 1;
    var scale = 'minor';
    const keys = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b'];
    Widget drop<T>(T v, List<(String, T)> items, void Function(T) set) => DropdownButton<T>(
          value: v,
          dropdownColor: cBg,
          style: const TextStyle(color: cN, fontSize: 13),
          items: [for (final (l, x) in items) DropdownMenuItem(value: x, child: Text(l))],
          onChanged: (x) => x == null ? null : set(x),
        );
    return showDialog<List<PatternRow>>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, set) => AlertDialog(
          backgroundColor: cBg,
          title: const Text('rows from a scale', style: TextStyle(color: cN, fontSize: 15)),
          content: Wrap(spacing: 14, crossAxisAlignment: WrapCrossAlignment.center, children: [
            drop(key, [for (var k = 0; k < 12; k++) (keys[k], k)], (v) => set(() => key = v)),
            drop(scale, [for (final s in kRowScales.keys) (s, s)], (v) => set(() => scale = v)),
            const Text('from octave', style: TextStyle(color: cGrey, fontSize: 12)),
            drop(octave, [for (var o = 0; o <= 7; o++) ('$o', o)], (v) => set(() => octave = v)),
            drop(octaves, const [('1 octave', 1), ('2 octaves', 2)], (v) => set(() => octaves = v)),
          ]),
          actions: [
            TextButton(
                onPressed: () => Navigator.of(context).pop(scaleRows(key, scale, octave, octaves)),
                child: const Text('ok')),
          ],
        ),
      ),
    );
  }

  Future<void> _addRow(Offset at) async {
    final have = p.rows.map((r) => r.note).toSet();
    if (widget.track.channel == 9 || p.rows.any((r) => Pads.drum(r.note) == r.name && r.name.isNotEmpty)) {
      final v = await _menu(at, [
        for (var n = 35; n <= 57; n++)
          if (!have.contains(n)) ('${Pads.drum(n)} (${pitchName(n)})', n),
      ]);
      if (v != null) {
        _edit(() {
          p.rows.insert(0, PatternRow(v, Pads.drum(v)));
          p.cells.insert(0, List<Step?>.filled(kMaxSteps, null));
        });
      }
      return;
    }
    final top = p.rows.isEmpty ? 59 : p.rows.map((r) => r.note).reduce(math.max);
    final n = math.min(127, top + 1);
    _edit(() {
      p.rows.insert(0, PatternRow(n, pitchName(n)));
      p.cells.insert(0, List<Step?>.filled(kMaxSteps, null));
    });
  }

  // Rows from a set; steps on rows of the same note stay
  void _rows(List<PatternRow> rows) {
    final old = {for (var r = 0; r < p.rows.length; r++) p.rows[r].note: p.cells[r]};
    while (p.rows.isNotEmpty) {
      p.removeRow(0);
    }
    for (final r in rows) {
      p.addRow(r);
      final keep = old[r.note];
      if (keep != null) p.cells.last.setAll(0, keep);
    }
    row = 0;
    topRow = 0;
  }

  @override
  Widget build(BuildContext context) {
    if (follow && widget.playStep >= 0 && paged) {
      final pg = widget.playStep ~/ 16;
      if (pg != page && pg < pages) page = pg;
    }
    if (page >= pages) page = pages - 1;
    row = row.clamp(0, math.max(0, p.rows.length - 1));
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      SizedBox(
        height: 34,
        child: SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          child: Row(children: [
            stepper('steps', '${p.steps}', () => _edit(() => p.steps = (p.steps - 1).clamp(1, kMaxSteps)),
                () => _edit(() => p.steps = (p.steps + 1).clamp(1, kMaxSteps))),
            chip('double', false, p.steps * 2 <= kMaxSteps ? () => _edit(() => doublePattern(p)) : null),
            const SizedBox(width: 14),
            const Text('step ', style: TextStyle(color: cGrey, fontSize: 12)),
            for (final (label, b) in const [
              ('1/8', 0.5),
              ('1/16', 0.25),
              ('1/32', 0.125),
              ('1/8T', 1 / 3),
              ('1/16T', 1 / 6)
            ])
              Padding(
                  padding: const EdgeInsets.only(right: 3),
                  child: chip(label, (p.stepBeats - b).abs() < 1e-9, () => _edit(() => p.stepBeats = b))),
            const SizedBox(width: 10),
            stepper(
                'swing',
                '${(p.swing * 100).round()}%',
                () => _edit(() => p.swing = (p.swing - 0.02).clamp(0.5, 0.75)),
                () => _edit(() => p.swing = (p.swing + 0.02).clamp(0.5, 0.75))),
            const SizedBox(width: 6),
            Builder(
                builder: (context) => GestureDetector(
                    onTapDown: (d) => _rowsMenu(d.globalPosition), child: chip('rows ▾', false, () {}))),
            const SizedBox(width: 4),
            Builder(
                builder: (context) =>
                    GestureDetector(onTapDown: (d) => _addRow(d.globalPosition), child: chip('+ row', false, () {}))),
            const SizedBox(width: 14),
            const Text('shift ', style: TextStyle(color: cGrey, fontSize: 12)),
            chip('◀', false, () => _edit(() => rotateSteps(p, -1))),
            const SizedBox(width: 3),
            chip('▶', false, () => _edit(() => rotateSteps(p, 1))),
            if (paged) ...[
              const SizedBox(width: 14),
              const Text('page ', style: TextStyle(color: cGrey, fontSize: 12)),
              for (var i = 0; i < pages; i++)
                Padding(
                    padding: const EdgeInsets.only(right: 3),
                    child: chip('${i + 1}', page == i, () => setState(() => page = i))),
              chip('follow', follow, () => setState(() => follow = !follow)),
            ],
          ]),
        ),
      ),
      const SizedBox(height: 4),
      Expanded(
        child: LayoutBuilder(builder: (context, box) {
          _cellsW = box.maxWidth - _labelW;
          _rowH = (box.maxHeight / math.max(1, p.rows.length)).clamp(16.0, 28.0);
          return Listener(
            key: const ValueKey('grid'),
            behavior: HitTestBehavior.opaque,
            onPointerDown: _gridDown,
            onPointerMove: _gridMove,
            onPointerUp: _gridUp,
            onPointerCancel: _gridUp,
            onPointerSignal: _scroll,
            child: CustomPaint(size: Size.infinite, painter: _GridPainter(this)),
          );
        }),
      ),
      const SizedBox(height: 4),
      SizedBox(
        height: 26,
        child: SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          child: Row(children: [
            SizedBox(
              width: _labelW,
              child: Text(p.rows.isEmpty ? '' : 'row: ${p.rows[row].name}',
                  style: const TextStyle(color: cGrey, fontSize: 12), overflow: TextOverflow.ellipsis),
            ),
            for (final q in StepParam.values)
              Padding(
                  padding: const EdgeInsets.only(right: 4),
                  child: chip(q.name, param == q, () => setState(() => param = q))),
          ]),
        ),
      ),
      SizedBox(
        height: _laneH,
        child: Listener(
          key: const ValueKey('lane'),
          behavior: HitTestBehavior.opaque,
          onPointerDown: (e) {
            _marked = false;
            _laneAt(e.localPosition, down: true);
          },
          onPointerMove: (e) => _laneAt(e.localPosition),
          child: CustomPaint(size: Size.infinite, painter: _LanePainter(this)),
        ),
      ),
    ]);
  }
}

void _text(Canvas c, String t, Offset at, {double size = 12, Color color = cN, double? maxW}) {
  final tp = TextPainter(
      text: TextSpan(text: t, style: TextStyle(color: color, fontSize: size)),
      textDirection: TextDirection.ltr,
      maxLines: 1,
      ellipsis: '…')
    ..layout(maxWidth: maxW ?? double.infinity);
  tp.paint(c, at);
}

class _GridPainter extends CustomPainter {
  final PatternEditorState s;
  _GridPainter(this.s);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    final p = s.p, rh = s._rowH, cw = s.cellW;
    final perBeat = math.max(1, (1 / p.stepBeats).round());
    for (var r = s.topRow; r < p.rows.length; r++) {
      final y = (r - s.topRow) * rh;
      if (y > size.height) break;
      final pr = p.rows[r], selected = r == s.row;
      if (selected) c.drawRect(Rect.fromLTWH(0, y, _labelW - 4, rh - 1), Paint()..color = cAc.withValues(alpha: 0.15));
      _text(c, pr.name, Offset(6, y + (rh - 15) / 2), color: pr.mute ? cGrey : cN, maxW: _labelW - 60);
      _text(c, pitchName(pr.note), Offset(_labelW - 54, y + (rh - 13) / 2), size: 10, color: cGrey);
      // M: mute
      final m = Rect.fromLTWH(_labelW - 22, y + (rh - 16) / 2, 16, 16);
      c.drawRect(m, Paint()..color = pr.mute ? cAc : const Color(0xFF1C2329));
      _text(c, 'M', Offset(m.left + 3, m.top + 1), size: 11, color: pr.mute ? cBg : cGrey);
      for (var st = s.first; st < s.first + s.shown; st++) {
        final x = s.cellX(st), here = st == s.widget.playStep;
        final cell = Rect.fromLTWH(x + 1, y + 1, cw - 2, rh - 2);
        final hue = stripHues[(st ~/ perBeat) % 3];
        final step = p.cells[r][st];
        c.drawRect(
            cell,
            Paint()
              ..color =
                  here ? cAc.withValues(alpha: 0.25) : hue.base.withValues(alpha: st % perBeat == 0 ? 0.26 : 0.15));
        if (step != null) {
          final v = down16to7(step.velocity) / 127;
          final fill = Rect.fromLTRB(cell.left, cell.bottom - cell.height * (0.25 + 0.75 * v), cell.right, cell.bottom);
          c.drawRect(fill, Paint()..color = (here ? cAc : hue.drag).withValues(alpha: pr.mute ? 0.35 : 1));
          if (step.probability < 1) {
            // a dot whose size is the chance
            c.drawCircle(Offset(cell.right - 5, cell.top + 5), 1.5 + 2.5 * step.probability, Paint()..color = cBg);
          }
          for (var h = 1; h < step.repeat; h++) {
            final hx = cell.left + cell.width * h / step.repeat;
            c.drawLine(
                Offset(hx, fill.top),
                Offset(hx, cell.bottom),
                Paint()
                  ..color = cBg
                  ..strokeWidth = 1.5);
          }
          if (step.nudge != 0) {
            final nx = cell.center.dx + step.nudge * cell.width;
            c.drawLine(
                Offset(nx, cell.top),
                Offset(nx, cell.top + 4),
                Paint()
                  ..color = cN
                  ..strokeWidth = 2);
          }
        }
      }
    }
    // beat lines
    for (var st = s.first; st <= s.first + s.shown; st++) {
      if (st % perBeat != 0) continue;
      final x = s.cellX(st);
      c.drawLine(
          Offset(x, 0),
          Offset(x, math.min(size.height, (p.rows.length - s.topRow) * rh)),
          Paint()
            ..color = cGrey.withValues(alpha: 0.55)
            ..strokeWidth = 1.5);
    }
  }

  @override
  bool shouldRepaint(_GridPainter old) => true;
}

class _LanePainter extends CustomPainter {
  final PatternEditorState s;
  _LanePainter(this.s);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    c.drawRect(Rect.fromLTWH(_labelW, 0, size.width - _labelW, size.height), Paint()..color = const Color(0xFF10171C));
    final p = s.p;
    if (p.rows.isEmpty) return;
    final (lo, hi) = s._range(s.param);
    final centre = s.param == StepParam.nudge;
    if (centre) {
      c.drawLine(Offset(_labelW, size.height / 2), Offset(size.width, size.height / 2),
          Paint()..color = cGrey.withValues(alpha: 0.4));
    }
    for (var st = s.first; st < s.first + s.shown; st++) {
      final step = p.cells[s.row][st];
      if (step == null) continue;
      final f = (s.value(step, s.param) - lo) / (hi - lo), x = s.cellX(st) + 3, w = s.cellW - 6;
      final top = size.height * (1 - f), base = centre ? size.height / 2 : size.height;
      c.drawRect(Rect.fromLTRB(x, math.min(top, base), x + w, math.max(top, base) + (centre ? 0 : 0)),
          Paint()..color = stripHues[1].drag);
      final label = switch (s.param) {
        StepParam.velocity => '${down16to7(step.velocity)}',
        StepParam.gate => step.gate.toStringAsFixed(2),
        StepParam.probability => '${(step.probability * 100).round()}%',
        StepParam.nudge => '${(step.nudge * 100).round()}',
        StepParam.repeat => '${step.repeat}x',
      };
      if (w >= 22) _text(c, label, Offset(x + 2, 2), size: 10, color: cGrey);
    }
  }

  @override
  bool shouldRepaint(_LanePainter old) => true;
}

Widget stepper(String label, String value, VoidCallback down, VoidCallback up) =>
    Row(mainAxisSize: MainAxisSize.min, children: [
      Text(label, style: const TextStyle(color: cGrey, fontSize: 12)),
      IconButton(
          onPressed: down, icon: const Icon(Icons.remove, size: 14), color: cAc, visualDensity: VisualDensity.compact),
      Text(value, style: const TextStyle(color: cN, fontSize: 13)),
      IconButton(
          onPressed: up, icon: const Icon(Icons.add, size: 14), color: cAc, visualDensity: VisualDensity.compact),
    ]);

Widget chip(String label, bool on, VoidCallback? tap) => GestureDetector(
      onTap: tap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
        decoration: BoxDecoration(color: on ? cAc : null, border: Border.all(color: on ? cAc : cGrey)),
        child: Text(label, style: TextStyle(color: on ? cBg : (tap == null ? cGrey : cN), fontSize: 12)),
      ),
    );

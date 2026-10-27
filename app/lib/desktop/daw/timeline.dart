// The arrangement's canvas (docs/daw.md "Clips on the arrangement"): the bar ruler and the tracks' lanes with their
// clips, drawn with CustomPainters and edited with raw pointer events, so one drag can be a move, a resize or a copy
// depending on where it starts and which keys are held.
//
// Ruler: a click sets where play starts, a drag sets the loop range. Lanes: drag a clip to move it (onto another
// track too), its edges to resize (the right edge repeats or cuts the content, the left trims its start), alt-drag
// to copy; a double-click opens it in the bottom panel; a double-click or right-click on empty space makes a clip.
// Scroll or two-finger pan to move, cmd-scroll or pinch to zoom.
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/shared/ui/theme.dart';

const kTrackH = 76.0, kRulerH = 28.0;

/// The view: how many pixels a beat is, which beat is at the left edge, how far the tracks are scrolled down.
class ArrangeView extends ChangeNotifier {
  double pxPerBeat = 28;
  double left = 0; // beats
  double top = 0; // px
  double grid = 1; // beats a drag snaps to; 0: off

  double x(double beat) => (beat - left) * pxPerBeat;
  double beatAt(double x) => left + x / pxPerBeat;

  double snap(double beat) => grid <= 0 ? beat : (beat / grid).round() * grid;

  void zoom(double factor, double aroundX) {
    final b = beatAt(aroundX);
    pxPerBeat = (pxPerBeat * factor).clamp(4.0, 400.0);
    left = math.max(0, b - aroundX / pxPerBeat);
    notifyListeners();
  }

  void scroll(double dx, double dy, double maxTop) {
    left = math.max(0, left + dx / pxPerBeat);
    top = (top + dy).clamp(0.0, math.max(0.0, maxTop));
    notifyListeners();
  }

  void changed() => notifyListeners();
}

/// What the canvas tells the page.
class ArrangeActions {
  final void Function(Clip?) select;
  final void Function(Clip) open; // a double-click on a clip
  final void Function(Track, double beat, Offset global) emptyMenu; // make a clip here
  final void Function(Clip, Offset global) clipMenu;
  const ArrangeActions({required this.select, required this.open, required this.emptyMenu, required this.clipMenu});
}

Color trackHue(int i) => stripHues[i % stripHues.length].base;

class Ruler extends StatefulWidget {
  final Project project;
  final ArrangeView view;
  final double Function() playhead; // song beats, or < 0 when stopped
  final VoidCallback changed;
  const Ruler({super.key, required this.project, required this.view, required this.playhead, required this.changed});

  @override
  State<Ruler> createState() => _RulerState();
}

class _RulerState extends State<Ruler> {
  double? _down; // beat where the pointer went down
  bool _dragged = false;

  @override
  Widget build(BuildContext context) {
    final v = widget.view, p = widget.project;
    return Listener(
      onPointerDown: (e) {
        _down = v.beatAt(e.localPosition.dx);
        _dragged = false;
      },
      onPointerMove: (e) {
        final d = _down;
        if (d == null) return;
        final b = v.beatAt(e.localPosition.dx);
        if (!_dragged && (b - d).abs() * v.pxPerBeat < 4) return;
        _dragged = true;
        final g = v.grid > 0 ? v.grid : 1.0;
        final a = (math.min(d, b) / g).floor() * g, z = (math.max(d, b) / g).ceil() * g;
        p
          ..loopStart = a
          ..loopEnd = math.max(z, a + g)
          ..loopOn = true;
        widget.changed();
      },
      onPointerUp: (e) {
        if (_down != null && !_dragged) {
          p.startBeat = math.max(0, v.snap(_down!));
          widget.changed();
        }
        _down = null;
      },
      child: CustomPaint(size: const Size(double.infinity, kRulerH), painter: _RulerPainter(p, v, widget.playhead())),
    );
  }
}

void _text(Canvas c, String t, Offset at, {double size = 11, Color color = cN}) {
  final tp = TextPainter(
      text: TextSpan(text: t, style: TextStyle(color: color, fontSize: size)), textDirection: TextDirection.ltr)
    ..layout();
  tp.paint(c, at);
}

class _RulerPainter extends CustomPainter {
  final Project p;
  final ArrangeView v;
  final double playhead;
  _RulerPainter(this.p, this.v, this.playhead);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    c.drawRect(Offset.zero & size, Paint()..color = const Color(0xFF10171C));
    if (p.loopEnd > p.loopStart) {
      c.drawRect(Rect.fromLTRB(v.x(p.loopStart), 0, v.x(p.loopEnd), size.height),
          Paint()..color = (p.loopOn ? cAc : cGrey).withValues(alpha: p.loopOn ? 0.35 : 0.18));
    }
    final bpb = p.beatsPerBar, barPx = v.pxPerBeat * bpb;
    final every = barPx >= 40 ? 1 : (40 / barPx).ceil(); // label every n bars when zoomed out
    final first = (v.left / bpb).floor(), last = (v.beatAt(size.width) / bpb).ceil();
    for (var bar = first; bar <= last; bar++) {
      final x = v.x(bar * bpb.toDouble());
      c.drawLine(Offset(x, size.height * 0.45), Offset(x, size.height), Paint()..color = cGrey);
      if (bar % every == 0) _text(c, '${bar + 1}', Offset(x + 3, 2));
      if (barPx >= 60) {
        for (var b = 1; b < bpb; b++) {
          final bx = x + b * v.pxPerBeat;
          c.drawLine(
              Offset(bx, size.height * 0.75), Offset(bx, size.height), Paint()..color = cGrey.withValues(alpha: 0.5));
        }
      }
    }
    // where play starts: a small triangle
    final sx = v.x(p.startBeat);
    c.drawPath(
        Path()
          ..moveTo(sx - 6, 0)
          ..lineTo(sx + 6, 0)
          ..lineTo(sx, 9)
          ..close(),
        Paint()..color = cN);
    if (playhead >= 0) {
      c.drawLine(
          Offset(v.x(playhead), 0),
          Offset(v.x(playhead), size.height),
          Paint()
            ..color = cAc
            ..strokeWidth = 2);
    }
  }

  @override
  bool shouldRepaint(_RulerPainter old) => true;
}

enum _Zone { body, left, right }

class Lanes extends StatefulWidget {
  final Project project;
  final History history;
  final ArrangeView view;
  final Clip? selected;
  final Set<Clip> recording;
  final double Function() playhead;
  final ArrangeActions actions;
  final VoidCallback changed;
  const Lanes(
      {super.key,
      required this.project,
      required this.history,
      required this.view,
      required this.selected,
      required this.recording,
      required this.playhead,
      required this.actions,
      required this.changed});

  @override
  State<Lanes> createState() => _LanesState();
}

class _LanesState extends State<Lanes> {
  // the drag under way
  Clip? _clip;
  _Zone _zone = _Zone.body;
  double _downBeat = 0, _start = 0, _length = 0, _offset = 0;
  bool _moved = false, _copy = false;
  // a double-click: two downs close in time and place
  DateTime _lastDown = DateTime(0);
  Offset _lastPos = Offset.zero;

  Project get p => widget.project;
  ArrangeView get v => widget.view;

  int _trackAt(double y) => ((y + v.top) / kTrackH).floor();

  (Clip, _Zone)? _hit(Offset at) {
    final ti = _trackAt(at.dy);
    if (ti < 0 || ti >= p.tracks.length) return null;
    // the last drawn is on top
    for (final c in p.tracks[ti].clips.reversed) {
      final l = v.x(c.start), r = v.x(c.end);
      if (at.dx < l || at.dx > r) continue;
      final edge = math.min(8.0, (r - l) / 4);
      if (at.dx - l < edge) return (c, _Zone.left);
      if (r - at.dx < edge) return (c, _Zone.right);
      return (c, _Zone.body);
    }
    return null;
  }

  void _down(PointerDownEvent e) {
    final now = DateTime.now();
    final twice = now.difference(_lastDown).inMilliseconds < 350 && (e.localPosition - _lastPos).distance < 6;
    _lastDown = now;
    _lastPos = e.localPosition;
    final hit = _hit(e.localPosition);
    final ti = _trackAt(e.localPosition.dy);
    if (e.buttons == kSecondaryMouseButton) {
      if (hit != null) {
        widget.actions.select(hit.$1);
        widget.actions.clipMenu(hit.$1, e.position);
      } else if (ti >= 0 && ti < p.tracks.length) {
        widget.actions.emptyMenu(p.tracks[ti], v.beatAt(e.localPosition.dx), e.position);
      }
      return;
    }
    if (hit == null) {
      widget.actions.select(null);
      if (twice && ti >= 0 && ti < p.tracks.length) {
        widget.actions.emptyMenu(p.tracks[ti], v.beatAt(e.localPosition.dx), e.position);
      }
      return;
    }
    final (c, zone) = hit;
    widget.actions.select(c);
    if (twice) {
      widget.actions.open(c);
      return;
    }
    _clip = c;
    _zone = zone;
    _downBeat = v.beatAt(e.localPosition.dx);
    _start = c.start;
    _length = c.length;
    _offset = c.offset;
    _moved = false;
    _copy = zone == _Zone.body && HardwareKeyboard.instance.isAltPressed;
  }

  void _move(PointerMoveEvent e) {
    var c = _clip;
    if (c == null) return;
    final d = v.beatAt(e.localPosition.dx) - _downBeat;
    if (!_moved && (d * v.pxPerBeat).abs() < 3 && (e.localPosition - _lastPos).distance < 3) return;
    if (!_moved) widget.history.mark(); // the first move: what was there before the drag
    if (!_moved && _copy) {
      // alt-drag: the original stays, a copy moves
      final copy = c.copy();
      p.trackOf(c)!.clips.add(copy);
      _clip = c = copy;
      widget.actions.select(copy);
    }
    _moved = true;
    final g = v.grid > 0 ? v.grid : 1 / 16;
    switch (_zone) {
      case _Zone.body:
        c.start = math.max(0, v.snap(_start + d));
        final ti = _trackAt(e.localPosition.dy).clamp(0, p.tracks.length - 1);
        final from = p.trackOf(c)!, to = p.tracks[ti];
        if (!identical(from, to)) {
          from.clips.remove(c);
          to.clips.add(c);
        }
      case _Zone.right:
        c.length = math.max(g, v.snap(_start + _length + d) - c.start);
      case _Zone.left:
        final end = _start + _length;
        final s = math.min(end - g, math.max(0.0, v.snap(_start + d)));
        c.start = s;
        c.length = end - s;
        final len = c.content.length;
        c.offset = len > 0 ? (_offset + (s - _start)) % len : 0;
        if (c.offset < 0) c.offset += len;
    }
    widget.changed();
  }

  void _up(PointerEvent e) => _clip = null;

  void _signal(PointerSignalEvent e) {
    if (e is! PointerScrollEvent) return;
    final keys = HardwareKeyboard.instance;
    if (keys.isMetaPressed || keys.isControlPressed) {
      v.zoom(math.exp(-e.scrollDelta.dy / 300), e.localPosition.dx);
    } else {
      v.scroll(e.scrollDelta.dx, e.scrollDelta.dy, _maxTop());
    }
  }

  double _maxTop() => p.tracks.length * kTrackH + kTrackH - (context.size?.height ?? 0);

  double _lastScale = 1;

  @override
  Widget build(BuildContext context) {
    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: _down,
      onPointerMove: _move,
      onPointerUp: _up,
      onPointerCancel: _up,
      onPointerSignal: _signal,
      onPointerPanZoomStart: (_) => _lastScale = 1,
      onPointerPanZoomUpdate: (e) {
        // the trackpad: two fingers scroll, a pinch zooms
        if ((e.scale - _lastScale).abs() > 1e-4) {
          v.zoom(e.scale / _lastScale, e.localPosition.dx);
          _lastScale = e.scale;
        } else {
          v.scroll(-e.panDelta.dx, -e.panDelta.dy, _maxTop());
        }
      },
      child: CustomPaint(
          size: Size.infinite, painter: _LanesPainter(p, v, widget.selected, widget.recording, widget.playhead())),
    );
  }
}

class _LanesPainter extends CustomPainter {
  final Project p;
  final ArrangeView v;
  final Clip? selected;
  final Set<Clip> recording;
  final double playhead;
  _LanesPainter(this.p, this.v, this.selected, this.recording, this.playhead);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    final bpb = p.beatsPerBar;
    // bars and beats behind everything
    final first = (v.left / bpb).floor(), last = (v.beatAt(size.width) / bpb).ceil();
    for (var bar = first; bar <= last; bar++) {
      final x = v.x(bar * bpb.toDouble());
      c.drawLine(Offset(x, 0), Offset(x, size.height), Paint()..color = cGrey.withValues(alpha: 0.30));
      if (v.pxPerBeat >= 15) {
        for (var b = 1; b < bpb; b++) {
          c.drawLine(Offset(x + b * v.pxPerBeat, 0), Offset(x + b * v.pxPerBeat, size.height),
              Paint()..color = cGrey.withValues(alpha: 0.10));
        }
      }
    }
    if (p.loopOn && p.loopEnd > p.loopStart) {
      c.drawRect(Rect.fromLTRB(v.x(p.loopStart), 0, v.x(p.loopEnd), size.height),
          Paint()..color = cAc.withValues(alpha: 0.05));
    }
    for (var i = 0; i < p.tracks.length; i++) {
      final y = i * kTrackH - v.top;
      if (y > size.height || y + kTrackH < 0) continue;
      c.drawLine(Offset(0, y + kTrackH), Offset(size.width, y + kTrackH), Paint()..color = const Color(0xFF1C2329));
      for (final clip in p.tracks[i].clips) {
        _clip(c, size, clip, i, y);
      }
    }
    if (playhead >= 0) {
      c.drawLine(
          Offset(v.x(playhead), 0),
          Offset(v.x(playhead), size.height),
          Paint()
            ..color = cAc
            ..strokeWidth = 2);
    }
  }

  void _clip(Canvas c, Size size, Clip clip, int track, double y) {
    final l = v.x(clip.start), r = v.x(clip.end);
    if (r < 0 || l > size.width) return;
    final rect = Rect.fromLTRB(l, y + 3, r, y + kTrackH - 3);
    final rec = recording.contains(clip);
    final hue = rec ? const Color(0xFFAB6D6C) : trackHue(track);
    c.drawRect(rect, Paint()..color = hue.withValues(alpha: 0.35));
    final head = Rect.fromLTRB(l, rect.top, r, rect.top + 15);
    c.drawRect(head, Paint()..color = hue.withValues(alpha: 0.8));
    c.save();
    c.clipRect(rect);
    _text(c, clip.name, Offset(l + 4, rect.top + 1), color: cBg);
    final body = Rect.fromLTRB(l, head.bottom + 2, r, rect.bottom - 2);
    final len = clip.content.length;
    if (len > 0) {
      // each pass through the content, from the clip's offset
      for (var k = 0;; k++) {
        final base = clip.start - clip.offset + k * len;
        if (base >= clip.end) break;
        if (k > 0 && base > clip.start) {
          final bx = v.x(base);
          c.drawLine(Offset(bx, body.top), Offset(bx, body.bottom), Paint()..color = cBg.withValues(alpha: 0.5));
        }
        _preview(c, clip.content, base, body);
        if (!clip.loops) break;
      }
    }
    c.restore();
    if (identical(clip, selected)) {
      c.drawRect(
          rect.deflate(1),
          Paint()
            ..color = cAc
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2);
    }
  }

  void _preview(Canvas c, Content content, double base, Rect body) {
    final paint = Paint()..color = cN.withValues(alpha: 0.75);
    if (content is MidiContent) {
      if (content.notes.isEmpty) return;
      var lo = 127, hi = 0;
      for (final n in content.notes) {
        lo = math.min(lo, n.pitch);
        hi = math.max(hi, n.pitch);
      }
      final rows = math.max(12, hi - lo + 1), rowH = body.height / rows;
      final bottom = body.bottom - (rows - (hi - lo + 1)) / 2 * rowH;
      for (final n in content.notes) {
        final x0 = v.x(base + n.start), x1 = math.max(x0 + 1.5, v.x(base + math.min(n.end, content.length)));
        final ny = bottom - (n.pitch - lo + 1) * rowH;
        c.drawRect(Rect.fromLTRB(x0, ny, x1, ny + math.max(1.5, rowH - 0.5)), paint);
      }
    } else if (content is Pattern) {
      final rows = content.rows.length;
      if (rows == 0) return;
      final rowH = body.height / rows;
      for (var r = 0; r < rows; r++) {
        for (var s = 0; s < content.steps; s++) {
          if (content.cells[r][s] == null) continue;
          final x0 = v.x(base + s * content.stepBeats);
          c.drawRect(
              Rect.fromLTWH(x0, body.top + r * rowH + rowH * 0.15, math.max(1.5, content.stepBeats * v.pxPerBeat * 0.6),
                  math.max(1.5, rowH * 0.7)),
              paint);
        }
      }
    }
  }

  @override
  bool shouldRepaint(_LanesPainter old) => true;
}

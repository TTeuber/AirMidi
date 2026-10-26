// Cutoff across, resonance up (lib/synth/groups.dart XyPad). The dot moves by the finger's travel, like the faders
// and the envelope: grabbing anywhere on the pad does not jump the sound (a drag across the whole height is the
// whole resonance range, though the dot only rides the top half, on the curve's peak). Double-tap for the defaults. Behind the dot,
// the shape of a resonant low-pass at that cutoff and resonance, drawn so its peak sits at the dot.
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../synth/controls.dart';
import '../synth/groups.dart';
import 'strips_page.dart';

class XyPadEditor extends StatefulWidget {
  final Device device;
  final XyPad pad;
  final void Function(Control c, int value) onSet;
  const XyPadEditor({super.key, required this.device, required this.pad, required this.onSet});

  @override
  State<XyPadEditor> createState() => _XyPadEditorState();
}

const _height = 200.0;

class _XyPadEditorState extends State<XyPadEditor> {
  Offset? last;
  DateTime lastDown = DateTime(0);

  void _down(PointerDownEvent e) {
    final now = DateTime.now();
    if (now.difference(lastDown).inMilliseconds < 300) {
      for (final c in widget.pad.controls) {
        widget.onSet(c, c.def);
      }
    }
    lastDown = now;
    setState(() => last = e.localPosition);
  }

  void _move(PointerMoveEvent e, double w) {
    final l = last;
    if (l == null) return;
    final d = e.localPosition - l;
    last = e.localPosition;
    final x = widget.pad.x, y = widget.pad.y;
    widget.onSet(x, x.value + (d.dx / w * (x.hi - x.lo)).round());
    widget.onSet(y, y.value - (d.dy / _height * (y.hi - y.lo)).round());
  }

  @override
  Widget build(BuildContext context) {
    final d = widget.device, p = widget.pad;
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        if (p.title.isNotEmpty) Text(p.title, style: const TextStyle(color: cN, fontSize: 13)),
        LayoutBuilder(builder: (context, box) {
          final w = box.maxWidth;
          return RawGestureDetector(
            gestures: {
              EagerGestureRecognizer: GestureRecognizerFactoryWithHandlers<EagerGestureRecognizer>(
                  () => EagerGestureRecognizer(), (_) {}),
            },
            child: Listener(
              onPointerDown: _down,
              onPointerMove: (e) => _move(e, w),
              onPointerUp: (_) => setState(() => last = null),
              onPointerCancel: (_) => setState(() => last = null),
              child: CustomPaint(
                  size: Size(w, _height), painter: _PadPainter(d.fraction(p.x), d.fraction(p.y), last != null)),
            ),
          );
        }),
        const SizedBox(height: 4),
        Row(children: [
          Expanded(child: _readout(p.x.title, display(d, p.x))),
          Expanded(child: _readout(p.y.title, display(d, p.y))),
        ]),
      ]),
    );
  }

  Widget _readout(String label, String value) => Column(children: [
        Text(label.toLowerCase(), style: TextStyle(color: last != null ? cAc : cGrey, fontSize: 11)),
        Text(value, style: TextStyle(color: last != null ? cAc : cN, fontSize: 15)),
      ]);
}

class _PadPainter extends CustomPainter {
  final double x, y;
  final bool active;
  _PadPainter(this.x, this.y, this.active);

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    canvas.drawRect(Offset.zero & size, Paint()..color = stripHues[1].base.withValues(alpha: 0.10));
    final grid = Paint()
      ..color = cGrey.withValues(alpha: 0.25)
      ..strokeWidth = 1;
    for (var i = 1; i < 4; i++) {
      canvas.drawLine(Offset(w * i / 4, 0), Offset(w * i / 4, h), grid);
      canvas.drawLine(Offset(0, h * i / 4), Offset(w, h * i / 4), grid);
    }

    // A 2-pole low-pass, ten octaves across the pad. Q = 0.707 .. 11 with resonance, so its peak (20 log10 Q dB)
    // rises from -3 to +21 dB. The scale runs from +21 dB at the top to -27 at the bottom: the roll-off shows below
    // the passband, and the dot (resonance over the top half) sits on the peak.
    final q = 0.7071 * math.pow(10, 1.2 * y);
    final path = Path();
    for (var px = 0.0; px <= w; px += 2) {
      final r = math.pow(2, (px / w - x) * 10).toDouble();
      final mag2 = 1 / (math.pow(1 - r * r, 2) + math.pow(r / q, 2));
      final db = 10 * math.log(mag2) / math.ln10;
      final py = ((21 - db) / 48 * h).clamp(-2.0, h + 2);
      px == 0 ? path.moveTo(px, py) : path.lineTo(px, py);
    }
    canvas.drawPath(Path.from(path)..lineTo(w, h)..lineTo(0, h)..close(),
        Paint()..color = stripHues[1].base.withValues(alpha: 0.35));
    canvas.drawPath(
        path,
        Paint()
          ..color = stripHues[1].drag
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2.5);

    final dot = Offset(x * w, (1 - y) * h / 2);
    canvas.drawCircle(dot, active ? 13 : 10, Paint()..color = active ? cAc : cBg);
    canvas.drawCircle(
        dot,
        active ? 13 : 10,
        Paint()
          ..color = active ? cAc : stripHues[1].drag
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2);
  }

  @override
  bool shouldRepaint(_PadPainter old) => old.x != x || old.y != y || old.active != active;
}

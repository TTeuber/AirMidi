// An envelope drawn from four controls (lib/synth/groups.dart): drag the peak for attack, the decay corner across
// for decay and up/down for sustain, the end of the sustain for sustain alone, the last point for release. Each
// handle moves by its finger's travel, like the faders: grabbing does not jump the value. Double-tap a handle for its
// default. Times come from the synth's own named values (its CtrlMapLists).
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import 'package:airmidi/shared/synth/controls.dart';
import 'package:airmidi/shared/synth/groups.dart';
import 'package:airmidi/shared/ui/theme.dart';

class EnvelopeEditor extends StatefulWidget {
  final Device device;
  final Envelope env;
  final void Function(Control c, int value) onSet;
  const EnvelopeEditor({super.key, required this.device, required this.env, required this.onSet});

  @override
  State<EnvelopeEditor> createState() => _EnvelopeEditorState();
}

const _pad = 18.0, _height = 170.0;

class _EnvelopeEditorState extends State<EnvelopeEditor> {
  Handle? active;
  Offset? last;
  Handle? lastTap;
  DateTime lastDown = DateTime(0);

  EnvShape _shape(double w) {
    final d = widget.device, e = widget.env;
    return EnvShape(w - 2 * _pad, _height - 2 * _pad, d.fraction(e.attack), d.fraction(e.decay), d.fraction(e.sustain),
        d.fraction(e.release));
  }

  List<Control> _of(Handle h) => switch (h) {
        Handle.attack => [widget.env.attack],
        Handle.decay => [widget.env.decay, widget.env.sustain],
        Handle.sustain => [widget.env.sustain],
        Handle.release => [widget.env.release],
      };

  void _down(PointerDownEvent e, double w) {
    final p = e.localPosition - const Offset(_pad, _pad);
    final h = nearestHandle(_shape(w), p.dx, p.dy);
    final now = DateTime.now();
    if (h == lastTap && now.difference(lastDown).inMilliseconds < 300) {
      for (final c in _of(h)) {
        widget.onSet(c, c.def);
      }
    }
    lastTap = h;
    lastDown = now;
    setState(() => (active = h, last = e.localPosition));
  }

  void _move(PointerMoveEvent e, double w) {
    final h = active, l = last;
    if (h == null || l == null) return;
    final s = _shape(w), d = e.localPosition - l;
    last = e.localPosition;
    void by(Control c, double frac) => widget.onSet(c, c.value + (frac * (c.hi - c.lo)).round());
    switch (h) {
      case Handle.attack:
        by(widget.env.attack, d.dx / s.seg);
      case Handle.decay:
        by(widget.env.decay, d.dx / s.seg);
        by(widget.env.sustain, -d.dy / s.h);
      case Handle.sustain:
        by(widget.env.sustain, -d.dy / s.h);
      case Handle.release:
        by(widget.env.release, d.dx / s.seg);
    }
  }

  void _up() => setState(() => (active = null, last = null));

  String _time(Control c) => display(widget.device, c);

  @override
  Widget build(BuildContext context) {
    final e = widget.env;
    final readouts = [
      ('attack', _time(e.attack), Handle.attack),
      ('decay', _time(e.decay), Handle.decay),
      ('sustain', display(widget.device, e.sustain), Handle.sustain),
      ('release', _time(e.release), Handle.release),
    ];
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        if (e.title.isNotEmpty) Text(e.title, style: const TextStyle(color: cN, fontSize: 13)),
        LayoutBuilder(builder: (context, box) {
          final w = box.maxWidth;
          // Claims the touch at once, so a vertical drag on sustain does not scroll the page.
          return RawGestureDetector(
            gestures: {
              EagerGestureRecognizer:
                  GestureRecognizerFactoryWithHandlers<EagerGestureRecognizer>(() => EagerGestureRecognizer(), (_) {}),
            },
            child: Listener(
              onPointerDown: (p) => _down(p, w),
              onPointerMove: (p) => _move(p, w),
              onPointerUp: (_) => _up(),
              onPointerCancel: (_) => _up(),
              child: CustomPaint(size: Size(w, _height), painter: _EnvPainter(_shape(w), active)),
            ),
          );
        }),
        const SizedBox(height: 4),
        Row(children: [
          for (final (label, value, h) in readouts)
            Expanded(
              child: Column(children: [
                Text(label, style: TextStyle(color: active == h ? cAc : cGrey, fontSize: 11)),
                Text(value, style: TextStyle(color: active == h ? cAc : cN, fontSize: 15)),
              ]),
            ),
        ]),
      ]),
    );
  }
}

class _EnvPainter extends CustomPainter {
  final EnvShape s;
  final Handle? active;
  _EnvPainter(this.s, this.active);

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = stripHues[0].base.withValues(alpha: 0.10));
    canvas.translate(_pad, _pad);
    final p = [for (final (x, y) in s.points) Offset(x, y)];
    canvas.drawLine(
        Offset(0, s.h),
        Offset(s.w, s.h),
        Paint()
          ..color = cGrey
          ..strokeWidth = 1);
    canvas.drawLine(
        Offset(0, 0),
        Offset(s.w, 0),
        Paint()
          ..color = cGrey.withValues(alpha: 0.4)
          ..strokeWidth = 1);

    // Decay and release bend like the exponential segments they are.
    final path = Path()
      ..moveTo(p[0].dx, p[0].dy)
      ..lineTo(p[1].dx, p[1].dy)
      ..quadraticBezierTo(p[1].dx, p[2].dy, p[2].dx, p[2].dy)
      ..lineTo(p[3].dx, p[3].dy)
      ..quadraticBezierTo(p[3].dx, p[4].dy, p[4].dx, p[4].dy);
    canvas.drawPath(Path.from(path)..close(), Paint()..color = stripHues[0].base.withValues(alpha: 0.35));
    canvas.drawPath(
        path,
        Paint()
          ..color = stripHues[0].drag
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2.5);

    for (var i = 0; i < 4; i++) {
      final on = active == Handle.values[i];
      canvas.drawCircle(p[i + 1], on ? 11 : 8, Paint()..color = on ? cAc : cBg);
      canvas.drawCircle(
          p[i + 1],
          on ? 11 : 8,
          Paint()
            ..color = on ? cAc : stripHues[0].drag
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2);
    }
  }

  @override
  bool shouldRepaint(_EnvPainter old) => true;
}

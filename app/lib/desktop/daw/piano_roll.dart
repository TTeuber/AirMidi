// The piano roll (docs/daw.md "The piano roll"): a MIDI clip's notes in the bottom panel, keys down the left, beats
// across, brighter for louder notes. For now it shows the clip; drawing and editing notes is A2.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/shared/ui/theme.dart';

class PianoRoll extends StatelessWidget {
  final MidiContent content;
  final double playBeat; // in the content, < 0 when not playing
  const PianoRoll({super.key, required this.content, this.playBeat = -1});

  @override
  Widget build(BuildContext context) => CustomPaint(size: Size.infinite, painter: _RollPainter(content, playBeat));
}

class _RollPainter extends CustomPainter {
  final MidiContent m;
  final double play;
  _RollPainter(this.m, this.play);

  @override
  void paint(Canvas c, Size size) {
    c.clipRect(Offset.zero & size);
    const keysW = 46.0;
    var lo = 60, hi = 72;
    for (final n in m.notes) {
      lo = math.min(lo, n.pitch);
      hi = math.max(hi, n.pitch);
    }
    lo = math.max(0, lo - 2);
    hi = math.min(127, hi + 2);
    final rows = hi - lo + 1, rowH = size.height / rows;
    final w = size.width - keysW, beatW = m.length > 0 ? w / m.length : w;
    double y(int pitch) => (hi - pitch) * rowH;
    for (var n = lo; n <= hi; n++) {
      final black = const [1, 3, 6, 8, 10].contains(n % 12);
      c.drawRect(Rect.fromLTWH(0, y(n), keysW - 2, rowH - 0.5),
          Paint()..color = black ? const Color(0xFF1C2329) : const Color(0xFFB9C2C7));
      c.drawRect(Rect.fromLTWH(keysW, y(n), w, rowH),
          Paint()..color = (black ? const Color(0xFF0D1317) : const Color(0xFF121A1F)));
      if (n % 12 == 0 && rowH >= 8) {
        final tp = TextPainter(
            text: TextSpan(text: pitchName(n), style: TextStyle(color: cBg, fontSize: math.min(10, rowH - 1))),
            textDirection: TextDirection.ltr)
          ..layout();
        tp.paint(c, Offset(keysW - tp.width - 4, y(n) + (rowH - tp.height) / 2));
      }
    }
    for (var b = 0; b <= m.length; b++) {
      final x = keysW + b * beatW;
      c.drawLine(
          Offset(x, 0), Offset(x, size.height), Paint()..color = cGrey.withValues(alpha: b % 4 == 0 ? 0.5 : 0.18));
    }
    for (final n in m.notes) {
      final x0 = keysW + n.start * beatW, x1 = keysW + math.min(n.end, m.length) * beatW;
      final a = 0.45 + 0.55 * down16to7(n.velocity) / 127;
      c.drawRect(Rect.fromLTRB(x0, y(n.pitch) + 1, math.max(x0 + 2, x1), y(n.pitch) + rowH - 1),
          Paint()..color = stripHues[0].drag.withValues(alpha: a));
      final bend = n.expression['bend'];
      if (bend != null && bend.length > 1) {
        // the note's own bend along it, as a line through the bar (half its height = a full bend)
        final path = Path();
        for (var i = 0; i < bend.length; i++) {
          final px = x0 + bend[i].t * beatW, py = y(n.pitch) + rowH / 2 - (bend[i].v / 0x100000000 - 0.5) * rowH * 4;
          i == 0 ? path.moveTo(px, py) : path.lineTo(px, py);
        }
        c.drawPath(
            path,
            Paint()
              ..color = cAc
              ..style = PaintingStyle.stroke
              ..strokeWidth = 1.2);
      }
    }
    if (play >= 0) {
      final x = keysW + play * beatW;
      c.drawLine(
          Offset(x, 0),
          Offset(x, size.height),
          Paint()
            ..color = cAc
            ..strokeWidth = 2);
    }
  }

  @override
  bool shouldRepaint(_RollPainter old) => true;
}

// A waveform drawn as an icon (one and a half cycles), for pickers whose values name shapes (groups.dart waveShapeOf).
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../synth/groups.dart';

class WaveIcon extends StatelessWidget {
  final WaveShape shape;
  final Color color;
  const WaveIcon(this.shape, {super.key, required this.color});

  @override
  Widget build(BuildContext context) => CustomPaint(size: const Size(44, 24), painter: _WavePainter(shape, color));
}

class _WavePainter extends CustomPainter {
  final WaveShape shape;
  final Color color;
  _WavePainter(this.shape, this.color);

  // -1..1 at phase t (cycles)
  double _at(double t) {
    final p = t - t.floorToDouble();
    switch (shape) {
      case WaveShape.sine:
        return math.sin(2 * math.pi * p);
      case WaveShape.triangle:
        return p < 0.25 ? 4 * p : (p < 0.75 ? 2 - 4 * p : 4 * p - 4);
      case WaveShape.saw:
        return p < 0.5 ? 2 * p : 2 * p - 2;
      case WaveShape.square:
        return p < 0.5 ? 1 : -1;
      case WaveShape.pulse:
        return p < 0.25 ? 1 : -1;
      case WaveShape.noise:
        return math.sin(t * 91.7) * math.cos(t * 37.3); // fixed, so the icon does not flicker
    }
  }

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height, cycles = 1.5;
    final path = Path();
    const n = 120;
    for (var i = 0; i <= n; i++) {
      final t = i / n * cycles;
      final px = i / n * w, py = h / 2 - _at(t) * h * 0.42;
      i == 0 ? path.moveTo(px, py) : path.lineTo(px, py);
    }
    canvas.drawPath(
        path,
        Paint()
          ..color = color
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2
          ..strokeJoin = StrokeJoin.round);
  }

  @override
  bool shouldRepaint(_WavePainter old) => old.shape != shape || old.color != color;
}

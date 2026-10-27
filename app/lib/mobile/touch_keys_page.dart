// Piano keys on the phone (docs/app.md): one keyboard across the screen in landscape, every finger its own note, a
// finger sliding onto another key plays that one, velocity from where the key is struck (towards the front is
// harder). Move mode, as on the strips: a drag scrolls the keys, a pinch across changes their width. Plain notes on
// channel 1, so the zone setup the strips use goes off first (an MPE Configuration Message with no members).
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'package:airmidi/mobile/surface_bar.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/midi/mpe.dart';
import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/strips/strips.dart' show Pinch, kPxPerMm;
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/theme.dart';

/// One send at a time, in order (a link takes a batch before the next).
class Sender {
  final MidiLink? link;
  Future<void> _q = Future.value();
  Sender(this.link);

  void send(List<List<int>> msgs) {
    final l = link;
    if (l == null || msgs.isEmpty) return;
    _q = _q.then((_) => l.send(msgs)).catchError((_) {});
  }
}

/// Where the keys are scrolled and how wide they are; the play page keeps it, so it survives a surface switch.
class KeysView {
  double width = 14; // mm, a white key
  double view = whiteIndex(60).toDouble(); // white keys up from note 0 at the left edge: c4
}

class TouchKeysPage extends StatefulWidget {
  final MidiLink? link;
  final SynthState? synth;
  final VoidCallback? onSurface;
  final KeysView? view;
  const TouchKeysPage({super.key, this.link, this.synth, this.onSurface, this.view});

  @override
  State<TouchKeysPage> createState() => _TouchKeysPageState();
}

class _TouchKeysPageState extends State<TouchKeysPage> with SingleTickerProviderStateMixin {
  final keys = Keys();
  late final out = Sender(widget.link);
  late final v = widget.view ?? KeysView();
  int lastVel = 0;
  bool moving = false; // move mode: fingers move the keys instead of playing
  final pinch = Pinch(12 * kPxPerMm); // fingers closer than 12 mm across do not change the width
  double? settleTo; // the whole white key the view glides to
  late final Ticker ticker;
  Duration? lastTick;
  double fieldW = 0;

  @override
  void initState() {
    super.initState();
    ticker = createTicker(_tick);
    out.send(mpeSetup(0)); // the strips' MPE zone off: one bend for all notes again
    final l = widget.link;
    if (l != null) {
      widget.synth?.attach(l);
      widget.synth?.load();
    }
  }

  @override
  void dispose() {
    ticker.dispose();
    out.send(keys.allOff());
    super.dispose();
  }

  void _tick(Duration now) {
    final dt = lastTick == null ? 1 / 60 : ((now - lastTick!).inMicroseconds / 1e6).clamp(0.001, 0.05);
    lastTick = now;
    final to = settleTo;
    if (to == null) {
      ticker.stop();
      lastTick = null;
      return;
    }
    setState(() {
      v.view += (to - v.view) * (1 - math.exp(-dt / 0.04));
      if ((to - v.view).abs() < 0.003) {
        v.view = to;
        settleTo = null;
      }
    });
  }

  void _glide(double to) {
    settleTo = to.clamp(0.0, math.max(0.0, kWhiteKeys - fieldW / (v.width * kPxPerMm))).roundToDouble();
    if (!ticker.isActive) ticker.start();
  }

  // an octave is seven white keys
  void _octave(int d) => setState(() {
        out.send(keys.allOff());
        _glide((settleTo ?? v.view) + 7 * d);
      });

  void _toggleMove() => setState(() {
        out.send(keys.allOff());
        pinch.clear();
        if (moving) _glide(settleTo ?? v.view);
        moving = !moving;
      });

  void _down(PointerDownEvent e, PianoView g) {
    if (moving) {
      settleTo = null;
      pinch.down(e.pointer, e.localPosition.dx, e.localPosition.dy);
      return;
    }
    _touch(e.pointer, e.localPosition, g, down: true);
  }

  void _move(PointerMoveEvent e, PianoView g) {
    if (moving) {
      final step = pinch.move(e.pointer, e.localPosition.dx, e.localPosition.dy);
      if (step == null) return;
      final (view, width) = movedKeys(v.view, v.width, g.w, kPxPerMm, step);
      setState(() {
        v.view = view;
        v.width = width;
      });
      return;
    }
    _touch(e.pointer, e.localPosition, g);
  }

  void _up(int pointer) {
    if (moving) {
      pinch.up(pointer);
      if (pinch.count == 0) _glide(v.view);
      return;
    }
    setState(() => out.send(keys.noteOff(pointer)));
  }

  void _touch(int pointer, Offset p, PianoView g, {bool down = false}) {
    final hit = g.hit(p.dx, p.dy);
    if (hit == null) return;
    final (n, vel) = hit;
    if (!down && keys.held[pointer]?.$1 == n) return; // still on the same key
    setState(() {
      lastVel = vel;
      out.send(keys.noteOn(pointer, n, vel: vel));
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: cBg,
      body: SafeArea(
        child: LayoutBuilder(builder: (context, outer) {
          // inset from the screen's edges: a slide that starts at an edge is Android's back gesture
          final w = outer.maxWidth - 20;
          fieldW = w;
          final g = PianoView(v.view, v.width * kPxPerMm, w, math.max(0.0, outer.maxHeight - 28 - kBarH));
          return Column(children: [
            SizedBox(
              height: 28,
              child: Center(
                child: Text(
                    moving
                        ? 'drag: along the keys   ·   pinch: key width (${v.width.round()} mm)'
                        : '${noteName(g.lo)} - ${noteName(g.hi)}   velocity $lastVel',
                    style: TextStyle(color: moving ? cAc : cN, fontSize: 13)),
              ),
            ),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 10),
              child: Listener(
                behavior: HitTestBehavior.opaque,
                onPointerDown: (e) => _down(e, g),
                onPointerMove: (e) => _move(e, g),
                onPointerUp: (e) => _up(e.pointer),
                onPointerCancel: (e) => _up(e.pointer),
                child: CustomPaint(size: Size(g.w, g.h), painter: _KeysPainter(g, keys.sounding, moving)),
              ),
            ),
            surfaceBar(context, 'keys', link: widget.link, synth: widget.synth, onSurface: widget.onSurface, extra: [
              barButton('oct -', () => _octave(-1)),
              barButton('oct +', () => _octave(1)),
              barButton(moving ? 'done' : 'move', _toggleMove, lit: moving),
            ]),
          ]);
        }),
      ),
    );
  }
}

class _KeysPainter extends CustomPainter {
  final PianoView g;
  final Set<int> on;
  final bool moving;
  _KeysPainter(this.g, this.on, this.moving);

  void _label(Canvas canvas, String t, Rect k, Color c) {
    final tp =
        TextPainter(text: TextSpan(text: t, style: TextStyle(color: c, fontSize: 12)), textDirection: TextDirection.ltr)
          ..layout();
    tp.paint(canvas, Offset(k.center.dx - tp.width / 2, k.bottom - tp.height - 8));
  }

  @override
  void paint(Canvas canvas, Size size) {
    canvas.clipRect(Offset.zero & size);
    final all = g.ww >= 34; // room for every white key's name
    for (var n = g.lo; n <= g.hi; n++) {
      if (isBlack(n)) continue;
      final k = g.rect(n).deflate(1.5);
      canvas.drawRect(k, Paint()..color = on.contains(n) ? cAc : const Color(0xFFB9C2C7));
      if (n % 12 == 0) {
        _label(canvas, noteName(n), k, cBg);
      } else if (all) {
        _label(canvas, noteName(n).replaceAll(RegExp(r'-?\d'), ''), k, cBg.withValues(alpha: 0.45));
      }
    }
    for (var n = g.lo; n <= g.hi; n++) {
      if (!isBlack(n)) continue;
      canvas.drawRect(g.rect(n), Paint()..color = on.contains(n) ? cAc : const Color(0xFF1C2329));
    }
    if (moving) {
      canvas.drawRect(
          (Offset.zero & size).deflate(1),
          Paint()
            ..color = cAc
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2);
    }
  }

  @override
  bool shouldRepaint(_KeysPainter old) => true;
}

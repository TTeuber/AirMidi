// Piano keys on the phone (docs/app.md): two octaves stacked, every finger its own note, a finger sliding onto
// another key plays that one, velocity from where the key is struck (towards the front is harder). Plain notes on
// channel 1, so the zone setup the strips use goes off first (an MPE Configuration Message with no members).
import 'package:flutter/material.dart';

import 'package:airmidi/mobile/surface_bar.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/midi/mpe.dart';
import 'package:airmidi/shared/play/keys.dart';
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

class TouchKeysPage extends StatefulWidget {
  final MidiLink? link;
  final SynthState? synth;
  final VoidCallback? onSurface;
  const TouchKeysPage({super.key, this.link, this.synth, this.onSurface});

  @override
  State<TouchKeysPage> createState() => _TouchKeysPageState();
}

class _TouchKeysPageState extends State<TouchKeysPage> {
  final keys = Keys();
  late final out = Sender(widget.link);
  int lastVel = 0;

  @override
  void initState() {
    super.initState();
    out.send(mpeSetup(0)); // the strips' MPE zone off: one bend for all notes again
    final l = widget.link;
    if (l != null) {
      widget.synth?.attach(l);
      widget.synth?.load();
    }
  }

  @override
  void dispose() {
    out.send(keys.allOff());
    super.dispose();
  }

  void _octave(int d) => setState(() {
        out.send(keys.allOff());
        keys.octave = (keys.octave + d).clamp(0, 7);
      });

  void _touch(int pointer, Offset p, KeyRows g, {bool down = false}) {
    final hit = g.hit(p.dx, p.dy);
    if (hit == null) return;
    final (n, v) = hit;
    if (!down && keys.held[pointer]?.$1 == n) return; // still on the same key
    setState(() {
      lastVel = v;
      out.send(keys.noteOn(pointer, n, vel: v));
    });
  }

  void _up(int pointer) => setState(() => out.send(keys.noteOff(pointer)));

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: cBg,
      body: SafeArea(
        child: Column(children: [
          SizedBox(
            height: 40,
            child: Center(
              child: Text('${noteName(keys.base)} - ${noteName(keys.base + 24)}   velocity $lastVel',
                  style: const TextStyle(color: cN, fontSize: 14)),
            ),
          ),
          Expanded(
            // inset from the screen's edges: a slide that starts at an edge is Android's back gesture
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 10),
              child: LayoutBuilder(builder: (context, box) {
                final g = KeyRows(keys.base, box.maxWidth, box.maxHeight);
                return Listener(
                  behavior: HitTestBehavior.opaque,
                  onPointerDown: (e) => _touch(e.pointer, e.localPosition, g, down: true),
                  onPointerMove: (e) => _touch(e.pointer, e.localPosition, g),
                  onPointerUp: (e) => _up(e.pointer),
                  onPointerCancel: (e) => _up(e.pointer),
                  child: CustomPaint(size: Size(box.maxWidth, box.maxHeight), painter: _KeysPainter(g, keys.sounding)),
                );
              }),
            ),
          ),
          surfaceBar(context, 'keys', link: widget.link, synth: widget.synth, onSurface: widget.onSurface, extra: [
            barButton('oct -', keys.octave > 0 ? () => _octave(-1) : null),
            barButton('oct +', keys.octave < 7 ? () => _octave(1) : null),
          ]),
        ]),
      ),
    );
  }
}

class _KeysPainter extends CustomPainter {
  final KeyRows g;
  final Set<int> on;
  _KeysPainter(this.g, this.on);

  @override
  void paint(Canvas canvas, Size size) {
    for (var r = 0; r < g.rows; r++) {
      final o = g.octaves[r], dy = r * g.rowH;
      canvas.save();
      canvas.translate(0, dy);
      for (final n in o.whites) {
        final k = o.rect(n).deflate(1.5);
        canvas.drawRect(k, Paint()..color = on.contains(n) ? cAc : const Color(0xFFB9C2C7));
        if (n % 12 == 0) {
          final tp = TextPainter(
              text: TextSpan(text: noteName(n), style: const TextStyle(color: cBg, fontSize: 12)),
              textDirection: TextDirection.ltr)
            ..layout();
          tp.paint(canvas, Offset(k.center.dx - tp.width / 2, k.bottom - tp.height - 8));
        }
      }
      for (var n = o.lo; n <= o.hi; n++) {
        if (!isBlack(n)) continue;
        canvas.drawRect(o.blackRect(n), Paint()..color = on.contains(n) ? cAc : const Color(0xFF1C2329));
      }
      canvas.restore();
    }
  }

  @override
  bool shouldRepaint(_KeysPainter old) => true;
}

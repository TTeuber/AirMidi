// Drum pads on the phone (docs/app.md): 4 x 4 on the General MIDI drum map, channel 10, kick at the bottom left;
// every finger its own pad, velocity from where it lands (the top is loud). "bank" moves all 16 by 16 notes.
import 'package:flutter/material.dart';

import 'package:airmidi/mobile/surface_bar.dart';
import 'package:airmidi/mobile/touch_keys_page.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/theme.dart';

class PadsPage extends StatefulWidget {
  final MidiLink? link;
  final SynthState? synth;
  final VoidCallback? onSurface;
  const PadsPage({super.key, this.link, this.synth, this.onSurface});

  @override
  State<PadsPage> createState() => _PadsPageState();
}

class _PadsPageState extends State<PadsPage> {
  final keys = Keys(); // one note per finger, each ended on its own channel
  final pads = Pads();
  late final out = Sender(widget.link);
  final down = <int, (int, int)>{}; // pointer -> pad (row, col)

  @override
  void dispose() {
    out.send(keys.allOff());
    super.dispose();
  }

  (int, int)? _padAt(Offset p, double side, double gap) {
    final c = (p.dx / (side + gap)).floor(), r = (p.dy / (side + gap)).floor();
    if (r < 0 || r > 3 || c < 0 || c > 3) return null;
    return (r, c);
  }

  void _down(PointerDownEvent e, double side, double gap) {
    final pad = _padAt(e.localPosition, side, gap);
    if (pad == null) return;
    final (r, c) = pad;
    final y = (e.localPosition.dy - r * (side + gap)) / side;
    setState(() {
      down[e.pointer] = pad;
      out.send(keys.noteOn(e.pointer, pads.note(r, c), vel: Pads.velocityAt(y), ch: pads.channel));
    });
  }

  void _up(int pointer) => setState(() {
        down.remove(pointer);
        out.send(keys.noteOff(pointer));
      });

  void _bank(int d) => setState(() {
        out.send(keys.allOff());
        pads.first = (pads.first + 16 * d).clamp(0, 112);
      });

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: cBg,
      body: SafeArea(
        child: Column(children: [
          SizedBox(
            height: 40,
            child: Center(
              child: Text('channel ${pads.channel + 1} · ${noteName(pads.first)} - ${noteName(pads.first + 15)}',
                  style: const TextStyle(color: cN, fontSize: 14)),
            ),
          ),
          Expanded(
            // inset from the screen's edges: a slide that starts at an edge is Android's back gesture
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 10),
              child: LayoutBuilder(builder: (context, box) {
                const gap = 8.0;
                final side = ((box.maxWidth - 3 * gap) / 4).clamp(20.0, (box.maxHeight - 3 * gap) / 4);
                final lit = down.values.toSet();
                return Center(
                  child: SizedBox(
                    width: side * 4 + gap * 3,
                    height: side * 4 + gap * 3,
                    child: Listener(
                      key: const ValueKey('pads'),
                      behavior: HitTestBehavior.opaque,
                      onPointerDown: (e) => _down(e, side, gap),
                      onPointerUp: (e) => _up(e.pointer),
                      onPointerCancel: (e) => _up(e.pointer),
                      child: Stack(children: [
                        for (var r = 0; r < 4; r++)
                          for (var c = 0; c < 4; c++)
                            Positioned(
                              left: c * (side + gap),
                              top: r * (side + gap),
                              width: side,
                              height: side,
                              child: _pad(r, c, lit.contains((r, c))),
                            ),
                      ]),
                    ),
                  ),
                );
              }),
            ),
          ),
          surfaceBar(context, 'pads', link: widget.link, synth: widget.synth, onSurface: widget.onSurface, extra: [
            barButton('bank -', pads.first > 0 ? () => _bank(-1) : null),
            barButton('bank +', pads.first < 112 ? () => _bank(1) : null),
          ]),
        ]),
      ),
    );
  }

  Widget _pad(int r, int c, bool on) {
    final n = pads.note(r, c), hue = stripHues[(r + c) % 3];
    return Container(
      color: on ? hue.drag : hue.base.withValues(alpha: 0.35),
      padding: const EdgeInsets.all(8),
      alignment: Alignment.bottomLeft,
      child: Text([Pads.drum(n), noteName(n)].where((s) => s.isNotEmpty).join('\n'),
          style: TextStyle(color: on ? cBg : cN, fontSize: 13)),
    );
  }
}

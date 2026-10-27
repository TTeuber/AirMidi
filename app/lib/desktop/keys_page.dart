// The desktop's way to play (docs/desktop.md D2): the computer keyboard and the mouse on a piano, pads, bend and mod
// wheel. The logic is in play/keys.dart; this page draws it and forwards keys and clicks. The strips are a button
// away (one mouse pointer plays one strip at a time).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/strips_page.dart';
import 'package:airmidi/shared/ui/synth_page.dart';

class KeysPage extends StatefulWidget {
  final MidiLink link;
  final SynthState? synth; // the box's MIDI-CI device; null without a box
  const KeysPage({super.key, required this.link, this.synth});

  @override
  State<KeysPage> createState() => _KeysPageState();
}

class _KeysPageState extends State<KeysPage> {
  final keys = Keys();
  final pads = Pads();
  final focus = FocusNode();
  late final AppLifecycleListener life;
  double bend = 0, mod = 0;
  bool bendDirty = false, modDirty = false;
  Timer? wheelTimer;
  String? error;
  (int, int)? padDown; // the pad the mouse holds

  @override
  void initState() {
    super.initState();
    // a key let go while another window has the keyboard never comes back here: end everything when we lose it
    life = AppLifecycleListener(onInactive: _allOff, onHide: _allOff);
  }

  @override
  void dispose() {
    _send(keys.allOff());
    wheelTimer?.cancel();
    life.dispose();
    focus.dispose();
    super.dispose();
  }

  void _allOff() => setState(() => _send(keys.allOff()));

  void _send(List<List<int>> msgs) {
    if (msgs.isEmpty) return;
    widget.link.send(msgs).catchError((e) {
      if (mounted) setState(() => error = '$e');
    });
  }

  KeyEventResult _key(FocusNode node, KeyEvent e) {
    final k = e.physicalKey;
    final ours = kKeyNotes.containsKey(k) ||
        const [
          PhysicalKeyboardKey.keyZ,
          PhysicalKeyboardKey.keyX,
          PhysicalKeyboardKey.keyC,
          PhysicalKeyboardKey.keyV,
          PhysicalKeyboardKey.tab,
        ].contains(k);
    if (!ours) return KeyEventResult.ignored;
    if (e is KeyDownEvent) setState(() => _send(keys.keyDown(k)));
    if (e is KeyUpEvent) setState(() => _send(keys.keyUp(k)));
    return KeyEventResult.handled; // repeats too, and Tab does not move the focus
  }

  // Bend and mod wheel: a drag moves them many times a frame; the newest value goes out every 8 ms.
  void _wheel({double? b, double? m}) {
    setState(() {
      if (b != null) {
        bend = b;
        bendDirty = true;
      }
      if (m != null) {
        mod = m;
        modDirty = true;
      }
    });
    wheelTimer ??= Timer(const Duration(milliseconds: 8), () {
      wheelTimer = null;
      _send([
        if (bendDirty) bendAt(keys.channel, bend),
        if (modDirty) [0xB0 | keys.channel, 1, (mod * 127).round()],
      ]);
      bendDirty = modDirty = false;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      focusNode: focus,
      autofocus: true,
      onKeyEvent: _key,
      child: Scaffold(
        appBar: AppBar(
          title: Text(widget.link.label, style: const TextStyle(fontSize: 16)),
          backgroundColor: cBg,
          actions: [
            if (widget.synth != null)
              TextButton(
                onPressed: () => Navigator.of(context)
                    .push(MaterialPageRoute(builder: (_) => SynthPage(synth: widget.synth!, link: widget.link))),
                child: const Text('synth'),
              ),
            TextButton(
              onPressed: () => Navigator.of(context)
                  .push(MaterialPageRoute(builder: (_) => StripsPage(link: widget.link, synth: widget.synth))),
              child: const Text('strips'),
            ),
          ],
        ),
        body: GestureDetector(
          behavior: HitTestBehavior.translucent,
          onTap: () => focus.requestFocus(), // a click anywhere gives the keyboard back to the piano
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              _controls(),
              const SizedBox(height: 6),
              const Text('A W S E D ... \' play  ·  Z X octave  ·  C V velocity  ·  Tab sustain',
                  style: TextStyle(color: cGrey, fontSize: 12)),
              if (error != null) Text(error!, style: const TextStyle(color: Color(0xFFAB6D6C), fontSize: 12)),
              const SizedBox(height: 12),
              SizedBox(height: 170, child: _piano()),
              const SizedBox(height: 16),
              Expanded(
                child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                  _wheelStrip('bend', bend, -1, 1, (v) => _wheel(b: v), springs: true),
                  const SizedBox(width: 10),
                  _wheelStrip('mod', mod, 0, 1, (v) => _wheel(m: v)),
                  const SizedBox(width: 20),
                  Expanded(child: _pads()),
                ]),
              ),
            ]),
          ),
        ),
      ),
    );
  }

  Widget _controls() {
    Widget step(String label, String value, VoidCallback down, VoidCallback up) => Padding(
          padding: const EdgeInsets.only(right: 22),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            Text(label, style: const TextStyle(color: cGrey, fontSize: 13)),
            IconButton(onPressed: down, icon: const Icon(Icons.remove, size: 16), color: cAc),
            SizedBox(width: 44, child: Center(child: Text(value, style: const TextStyle(color: cN, fontSize: 15)))),
            IconButton(onPressed: up, icon: const Icon(Icons.add, size: 16), color: cAc),
          ]),
        );
    void change(void Function() f) => setState(() {
          _send(keys.allOff()); // a note started on the old channel would never end
          f();
        });
    return Wrap(crossAxisAlignment: WrapCrossAlignment.center, children: [
      step('octave', noteName(keys.base), () => setState(() => keys.octave = (keys.octave - 1).clamp(-1, 8)),
          () => setState(() => keys.octave = (keys.octave + 1).clamp(-1, 8))),
      step('velocity', '${keys.velocity}', () => setState(() => keys.velocity = (keys.velocity - 20).clamp(1, 127)),
          () => setState(() => keys.velocity = (keys.velocity + 20).clamp(1, 127))),
      step('channel', '${keys.channel + 1}', () => change(() => keys.channel = (keys.channel - 1) % 16),
          () => change(() => keys.channel = (keys.channel + 1) % 16)),
      step('pads ch', '${pads.channel + 1}', () => change(() => pads.channel = (pads.channel - 1) % 16),
          () => change(() => pads.channel = (pads.channel + 1) % 16)),
      Text(keys.sustain ? 'sustain' : '', style: const TextStyle(color: cAc, fontSize: 13)),
    ]);
  }

  // Two octaves and the top c from the home row's c; the mouse plays one note at a time and slides across.
  Widget _piano() => LayoutBuilder(builder: (context, box) {
        final lo = keys.base;
        final geo = PianoGeometry(lo, lo + 24, box.maxWidth, box.maxHeight);
        void at(Offset p) {
          final n = geo.noteAt(p.dx, p.dy);
          if (n != null && keys.held['mouse']?.$1 != n) setState(() => _send(keys.noteOn('mouse', n)));
        }

        return Listener(
          onPointerDown: (e) => at(e.localPosition),
          onPointerMove: (e) => at(e.localPosition),
          onPointerUp: (_) => setState(() => _send(keys.noteOff('mouse'))),
          onPointerCancel: (_) => setState(() => _send(keys.noteOff('mouse'))),
          child: CustomPaint(size: Size(box.maxWidth, box.maxHeight), painter: _PianoPainter(geo, keys.sounding)),
        );
      });

  Widget _wheelStrip(String label, double v, double lo, double hi, ValueChanged<double> set, {bool springs = false}) =>
      SizedBox(
        width: 44,
        child: Column(children: [
          Expanded(
            child: LayoutBuilder(builder: (context, box) {
              double valueAt(double y) => (hi - (y / box.maxHeight).clamp(0.0, 1.0) * (hi - lo));
              final f = (v - lo) / (hi - lo);
              return Listener(
                onPointerDown: (e) => set(valueAt(e.localPosition.dy)),
                onPointerMove: (e) => set(valueAt(e.localPosition.dy)),
                onPointerUp: (_) => springs ? set(0) : null,
                child: Stack(children: [
                  Container(color: stripHues[1].base.withValues(alpha: 0.15)),
                  Positioned(
                    left: 0,
                    right: 0,
                    top: (1 - f) * box.maxHeight - 3,
                    child: Container(height: 6, color: stripHues[1].drag),
                  ),
                ]),
              );
            }),
          ),
          const SizedBox(height: 4),
          Text(label, style: const TextStyle(color: cGrey, fontSize: 11)),
        ]),
      );

  Widget _pads() => LayoutBuilder(builder: (context, box) {
        const gap = 8.0;
        final side = ((box.maxHeight - 3 * gap) / 4).clamp(20.0, (box.maxWidth - 3 * gap) / 4);
        return Align(
          alignment: Alignment.topLeft,
          child: SizedBox(
            width: side * 4 + gap * 3,
            child: Column(children: [
              for (var r = 0; r < 4; r++)
                Padding(
                  padding: EdgeInsets.only(bottom: r < 3 ? gap : 0),
                  child: Row(children: [
                    for (var c = 0; c < 4; c++)
                      Padding(padding: EdgeInsets.only(right: c < 3 ? gap : 0), child: _pad(r, c, side)),
                  ]),
                ),
            ]),
          ),
        );
      });

  Widget _pad(int r, int c, double side) {
    final note = pads.note(r, c), on = padDown == (r, c);
    final hue = stripHues[(r + c) % 3];
    return Listener(
      onPointerDown: (e) => setState(() {
        padDown = (r, c);
        _send(keys.noteOn(('pad', r, c), note, vel: Pads.velocityAt(e.localPosition.dy / side), ch: pads.channel));
      }),
      onPointerUp: (_) => setState(() {
        padDown = null;
        _send(keys.noteOff(('pad', r, c)));
      }),
      child: Container(
        width: side,
        height: side,
        color: on ? hue.drag : hue.base.withValues(alpha: 0.35),
        alignment: Alignment.bottomLeft,
        padding: const EdgeInsets.all(6),
        child: Text([Pads.drum(note), noteName(note)].where((s) => s.isNotEmpty).join('\n'),
            style: TextStyle(color: on ? cBg : cN, fontSize: 12)),
      ),
    );
  }
}

/// Where the keys of [lo]..[hi] are in a [w] x [h] box: white keys side by side, black keys on top over the top 60 %.
class PianoGeometry {
  final int lo, hi;
  final double w, h;
  late final List<int> whites = [
    for (var n = lo; n <= hi; n++)
      if (!isBlack(n)) n
  ];
  late final double ww = w / whites.length;
  PianoGeometry(this.lo, this.hi, this.w, this.h);

  double whiteX(int n) => whites.indexOf(n) * ww;

  /// A black key sits across the boundary after the white key below it.
  Rect blackRect(int n) => Rect.fromLTWH(whiteX(n - 1) + ww * 0.68, 0, ww * 0.64, h * 0.6);

  Rect rect(int n) => isBlack(n) ? blackRect(n) : Rect.fromLTWH(whiteX(n), 0, ww, h);

  int? noteAt(double x, double y) {
    if (x < 0 || x >= w || y < 0 || y > h) return null;
    for (var n = lo; n <= hi; n++) {
      if (isBlack(n) && blackRect(n).contains(Offset(x, y))) return n;
    }
    final i = (x / ww).floor().clamp(0, whites.length - 1);
    return whites[i];
  }
}

class _PianoPainter extends CustomPainter {
  final PianoGeometry g;
  final Set<int> on;
  _PianoPainter(this.g, this.on);

  @override
  void paint(Canvas canvas, Size size) {
    void label(int n, Rect r, Color c) {
      final semis = n - g.lo;
      final t = semis >= 0 && semis < kKeyLabels.length ? kKeyLabels[semis] : '';
      final name = n % 12 == 0 ? noteName(n) : '';
      final tp = TextPainter(
        text: TextSpan(text: [t, name].where((s) => s.isNotEmpty).join('\n'), style: TextStyle(color: c, fontSize: 11)),
        textAlign: TextAlign.center,
        textDirection: TextDirection.ltr,
      )..layout(minWidth: r.width, maxWidth: r.width);
      tp.paint(canvas, Offset(r.left, r.bottom - tp.height - 6));
    }

    for (final n in g.whites) {
      final r = g.rect(n).deflate(1);
      canvas.drawRect(r, Paint()..color = on.contains(n) ? cAc : const Color(0xFFB9C2C7));
      label(n, r, cBg);
    }
    for (var n = g.lo; n <= g.hi; n++) {
      if (!isBlack(n)) continue;
      final r = g.blackRect(n);
      canvas.drawRect(r, Paint()..color = on.contains(n) ? cAc : const Color(0xFF1C2329));
      label(n, r, on.contains(n) ? cBg : cN);
    }
  }

  @override
  bool shouldRepaint(_PianoPainter old) => true;
}

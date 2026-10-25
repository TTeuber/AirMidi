// The strips: raw pointers in (Listener, every finger with its id), drawing with a CustomPainter, events to the
// Player. Portrait, full screen. Colours from the prototype's forslike palette.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import '../link/link.dart';
import '../play/player.dart';
import '../strips/strips.dart';
import '../synth/state.dart';
import 'synth_page.dart';

const cBg = Color(0xFF090C0F), cN = Color(0xFFA5B0B6), cGrey = Color(0xFF636363), cAc = Color(0xFFE9C944);
const stripHues = [
  (base: Color(0xFF5C8E67), drag: Color(0xFF86B990)), // fir
  (base: Color(0xFF5187AB), drag: Color(0xFF7BB2D9)), // slate
  (base: Color(0xFF96719E), drag: Color(0xFFC29BCB)), // heather
];

class Settings {
  double row = 11, width = 15, gap = 3; // mm; the Pixel 9a is ~65 mm wide in portrait
  int count = 3;
  List<int> tuning = defaultTuning(3);
  bool snap = false, legato = true;
  bool showStats = false; // the link's numbers above the bar (for measuring)
  double assist = 0, pull = 150; // 0..100, ms
  int key = 7;
  String scale = 'off';

  LayoutSettings get layout => LayoutSettings(row: row, width: width, gap: gap, align: StripAlign.mid, tuning: tuning);
}

class StripsPage extends StatefulWidget {
  final MidiLink? link;
  final SynthState? synth; // the box's MIDI-CI device, for the synth page; null without a box
  const StripsPage({super.key, this.link, this.synth});

  @override
  State<StripsPage> createState() => _StripsPageState();
}

class _StripsPageState extends State<StripsPage> with SingleTickerProviderStateMixin {
  final s = Settings();
  final strings = Strings();
  late final Player player = Player(legato: () => s.legato);
  final repaint = ValueNotifier(0);
  late final Ticker ticker = createTicker(_tick);
  Duration? lastTick;
  StripLayout? L;
  Timer? statsTimer;
  bool showSettings = false;

  static const _readH = 64.0; // px, the note readouts above the strips

  @override
  void initState() {
    super.initState();
    SystemChrome.setPreferredOrientations([DeviceOrientation.portraitUp]);
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    _setAssist();
    final l = widget.link;
    if (l != null) {
      player.attach(l, s.count).catchError((e) => setState(() => player.lastError = '$e'));
      widget.synth?.attach(l); // the device's knobs move its values while the synth page is closed, too
      widget.synth?.load();
    }
    statsTimer = Timer.periodic(const Duration(seconds: 1), (_) => setState(() {}));
  }

  @override
  void dispose() {
    statsTimer?.cancel();
    ticker.dispose();
    player.dispose();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    super.dispose();
  }

  void _setAssist() => strings.assist = Assist(s.assist / 100, s.pull / 1000);

  // The assist evolves while a finger rests, with no pointer events to drive it: advance it every frame.
  void _tick(Duration now) {
    final dt = lastTick == null ? 1 / 60 : ((now - lastTick!).inMicroseconds / 1e6).clamp(0.001, 0.05);
    lastTick = now;
    player.handleAll(strings.tick(dt));
    repaint.value++;
    if (strings.touchCount == 0) {
      ticker.stop();
      lastTick = null;
    }
  }

  void _down(PointerDownEvent e) {
    final l = L;
    if (l == null || e.localPosition.dy < l.top) return;
    final st = l.stripAt(e.localPosition.dx);
    if (st == null) return;
    player.handle(strings.down(e.pointer, st.i, l.pitchAt(st, e.localPosition.dy), s.snap));
    if (!ticker.isActive) ticker.start();
    repaint.value++;
  }

  void _move(PointerMoveEvent e) {
    final l = L, strip = strings.stripOf(e.pointer);
    if (l == null || strip == null || strip >= l.strips.length) return;
    player.handle(strings.move(e.pointer, l.pitchAt(l.strips[strip], e.localPosition.dy)));
    repaint.value++;
  }

  void _up(PointerEvent e) {
    player.handle(strings.up(e.pointer));
    repaint.value++;
  }

  // Geometry changes drop the fingers (their strips moved); only a new strip count changes the MPE zone, so only
  // that sends the zone setup again. Every setup makes the synth cut its notes, and a slider sends dozens of ticks.
  void _relayout(VoidCallback f) {
    final count = s.count;
    player.handleAll(strings.clear());
    setState(f);
    final l = widget.link;
    if (l != null && s.count != count) player.attach(l, s.count);
  }

  static const _barH = 56.0;

  @override
  Widget build(BuildContext context) {
    final st = player.shown;
    return Scaffold(
      backgroundColor: cBg,
      body: SafeArea(
        child: Stack(children: [
          LayoutBuilder(builder: (context, box) {
            L = layout(0, _readH, box.maxWidth, box.maxHeight - _readH - _barH - (s.showStats ? _statsH : 0), s.layout);
            return Listener(
              behavior: HitTestBehavior.opaque,
              onPointerDown: _down,
              onPointerMove: _move,
              onPointerUp: _up,
              onPointerCancel: _up,
              child: CustomPaint(size: Size(box.maxWidth, box.maxHeight), painter: _Painter(this)),
            );
          }),
          if (s.showStats)
            Positioned(
              left: 10,
              right: 10,
              bottom: _barH,
              height: _statsH,
              child: Text(
                '${widget.link?.label ?? 'no box'}\n'
                '${st.batches}/s · ${st.messages} msg/s · '
                'send ${st.sendMsMean.toStringAsFixed(1)}/${st.sendMsMax.toStringAsFixed(1)} · '
                'wait ${st.waitMsMean.toStringAsFixed(1)}/${st.waitMsMax.toStringAsFixed(1)} ms'
                '${player.stats.errors > 0 ? ' · ${player.stats.errors} errors' : ''}',
                style: const TextStyle(color: cN, fontSize: 10, height: 1.4),
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          Positioned(
            left: 4,
            right: 4,
            bottom: 0,
            height: _barH,
            child: Row(children: [
              _barButton('box', () => Navigator.of(context).maybePop()),
              _barButton('synth', _openSynth, enabled: widget.synth != null && widget.link != null),
              _barButton(showSettings ? 'done' : 'layout', () => setState(() => showSettings = !showSettings)),
            ]),
          ),
          if (showSettings) Positioned(left: 0, right: 0, bottom: _barH, child: _settings()),
        ]),
      ),
    );
  }

  static const _statsH = 30.0;

  Future<void> _openSynth() async {
    setState(() => showSettings = false);
    player.handleAll(strings.clear()); // fingers off the strips while the page is up
    await Navigator.of(context)
        .push(MaterialPageRoute(builder: (_) => SynthPage(synth: widget.synth!, link: widget.link!)));
  }

  // A third of the bar each; the whole bar height responds, not just the word.
  Widget _barButton(String label, VoidCallback onTap, {bool enabled = true}) => Expanded(
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: enabled ? onTap : null,
          child: SizedBox(
            height: _barH,
            child: Center(
              child: Container(
                height: 40,
                margin: const EdgeInsets.symmetric(horizontal: 4),
                alignment: Alignment.center,
                decoration: BoxDecoration(border: Border.all(color: enabled ? cAc : cGrey, width: 1.5)),
                child: Text(label, style: TextStyle(color: enabled ? cAc : cGrey, fontSize: 15)),
              ),
            ),
          ),
        ),
      );

  Widget _slider(String label, double v, double min, double max, ValueChanged<double> f, {String unit = ''}) {
    return Row(children: [
      SizedBox(width: 56, child: Text(label, style: const TextStyle(color: cN, fontSize: 12))),
      Expanded(
        child: Slider(value: v.clamp(min, max), min: min, max: max, activeColor: cAc, inactiveColor: cGrey, onChanged: f),
      ),
      SizedBox(width: 52, child: Text('${v.round()}$unit', style: const TextStyle(color: cN, fontSize: 12))),
    ]);
  }

  Widget _toggle(String label, bool v, ValueChanged<bool> f) => Row(mainAxisSize: MainAxisSize.min, children: [
        Text(label, style: const TextStyle(color: cN, fontSize: 12)),
        Switch(value: v, activeColor: cAc, onChanged: f),
      ]);

  // A square cell that fills with the accent colour when selected (the forslike tile, small).
  Widget _cell(String label, bool selected, VoidCallback onTap, {double width = 44}) => GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Container(
          width: width,
          height: 40,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: selected ? cAc : Colors.transparent,
            border: Border.all(color: selected ? cAc : cGrey),
          ),
          child: Text(label, style: TextStyle(color: selected ? cBg : cN, fontSize: 14)),
        ),
      );

  Widget _settings() {
    final keyed = s.scale != 'off';
    return Container(
      color: const Color(0xF20A1014),
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 10),
      child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
        _slider('row', s.row, 6, 30, (v) => _relayout(() => s.row = v.roundToDouble()), unit: ' mm'),
        _slider('width', s.width, 8, 40, (v) => _relayout(() => s.width = v.roundToDouble()), unit: ' mm'),
        _slider('gap', s.gap, 0, 20, (v) => _relayout(() => s.gap = v.roundToDouble()), unit: ' mm'),
        _slider('strips', s.count.toDouble(), 1, 6, (v) {
          if (v.round() == s.count) return;
          _relayout(() {
            s.count = v.round();
            s.tuning = defaultTuning(s.count);
          });
        }),
        _slider('assist', s.assist, 0, 100, (v) => setState(() {
              s.assist = v;
              _setAssist();
            })),
        _slider('pull', s.pull, 20, 1000, (v) => setState(() {
              s.pull = v;
              _setAssist();
            }), unit: ' ms'),
        const SizedBox(height: 6),
        const Text('scale', style: TextStyle(color: cN, fontSize: 12)),
        const SizedBox(height: 4),
        Wrap(spacing: 6, runSpacing: 6, children: [
          for (final sc in ['off', 'maj', 'min', 'pent'])
            _cell(sc, s.scale == sc, () => setState(() => s.scale = sc), width: 60),
        ]),
        const SizedBox(height: 8),
        Text('key', style: TextStyle(color: cN.withValues(alpha: keyed ? 1 : 0.45), fontSize: 12)),
        const SizedBox(height: 4),
        Opacity(
          opacity: keyed ? 1 : 0.45,
          child: Wrap(spacing: 6, runSpacing: 6, children: [
            for (var k = 0; k < 12; k++)
              _cell(kNames[k], s.key == k, () => setState(() {
                    s.key = k;
                    if (s.scale == 'off') s.scale = 'maj'; // picking a key turns the guide on
                  })),
          ]),
        ),
        const SizedBox(height: 4),
        Wrap(spacing: 12, crossAxisAlignment: WrapCrossAlignment.center, children: [
          _toggle('snap', s.snap, (v) => setState(() => s.snap = v)),
          _toggle('legato', s.legato, (v) => setState(() => s.legato = v)),
          _toggle('link stats', s.showStats, (v) => setState(() => s.showStats = v)),
          _cell('all off', false, () => player.allOff(), width: 80),
        ]),
      ]),
    );
  }
}

class _Painter extends CustomPainter {
  final _StripsPageState p;
  _Painter(this.p) : super(repaint: p.repaint);

  void _text(Canvas c, String t, Offset centre, double size, Color col) {
    final tp = TextPainter(
      text: TextSpan(text: t, style: TextStyle(color: col, fontSize: size, fontWeight: FontWeight.w500)),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(c, centre - Offset(tp.width / 2, tp.height / 2));
  }

  @override
  void paint(Canvas c, Size size) {
    final L = p.L, s = p.s;
    if (L == null) return;
    const gapY = 2.0; // background between rows separates them; no lines
    final rows = ((L.bottom - L.top) / L.row).ceil();
    final labelSize = L.row >= 40 ? 15.0 : L.row >= 28 ? 12.0 : 0.0;
    final paint = Paint();
    for (final st in L.strips) {
      final hue = stripHues[st.i % stripHues.length], state = p.strings.state(st.i);
      final fillY = state == null ? L.top : L.yAt(st, state.pitch).clamp(L.top, L.bottom);
      for (var r = 0; r < rows; r++) {
        final y0 = L.top + r * L.row, h = (L.row < L.bottom - y0 ? L.row : L.bottom - y0) - gapY, note = st.base + r;
        if (h < 1) continue;
        final strong = s.scale == 'off' ? r % 12 == 0 : isRoot(note, s.key);
        paint.color = !inKey(note, s.key, s.scale)
            ? cGrey.withValues(alpha: 0.1)
            : hue.base.withValues(alpha: strong ? 0.3 : 0.18);
        c.drawRect(Rect.fromLTWH(st.x, y0 + gapY / 2, st.w, h), paint);
        if (labelSize > 0 && y0 + L.row / 2 + labelSize / 2 < L.bottom) {
          final filled = y0 + L.row / 2 < fillY;
          final a = inKey(note, s.key, s.scale) ? (filled ? 1.0 : 0.45) : (filled ? 0.45 : 0.18);
          _text(c, kNames[note % 12], Offset(st.x + st.w / 2, y0 + L.row / 2), labelSize,
              (filled ? cBg : cN).withValues(alpha: a));
        }
      }
      if (state != null) {
        // The strip fills down to the sounding pitch, like a tile filling to its value.
        paint.color = hue.base.withValues(alpha: 0.85);
        c.drawRect(Rect.fromLTRB(st.x, L.top, st.x + st.w, fillY), paint);
        paint.color = hue.drag;
        for (final h in state.held) {
          c.drawRect(Rect.fromLTWH(st.x, L.yAt(st, h) - 1.5, st.w, 3), paint);
        }
      }
      // readout: nearest note, cents, an underline within 5 cents
      final n = nearest(state?.pitch ?? st.base.toDouble()), cx = st.x + st.w / 2;
      _text(c, noteName(n.note), Offset(cx, 20), 20, cN.withValues(alpha: state == null ? 0.45 : 1));
      if (state != null) {
        if (n.cents.abs() <= 5) {
          paint.color = hue.base;
          c.drawRect(Rect.fromCenter(center: Offset(cx, 34), width: 24, height: 2), paint);
        }
        _text(c, '${n.cents > 0 ? '+' : ''}${n.cents}', Offset(cx, 48), 12, cN.withValues(alpha: 0.7));
      }
    }
    // fret markers
    paint.color = cN.withValues(alpha: 0.45);
    const side = 7.0;
    kFretMarkers.forEach((fret, n) {
      final y = L.top + (fret + 0.5) * L.row;
      if (y + side / 2 > L.bottom) return;
      for (var k = 0; k < n; k++) {
        final x = L.markerX + (n == 2 ? (k == 1 ? 0.75 : -0.75) * side : 0);
        c.drawRect(Rect.fromCenter(center: Offset(x, y), width: side, height: side), paint);
      }
    });
  }

  @override
  bool shouldRepaint(_Painter old) => true;
}

// The synth behind the box: its controls as its MIDI-CI control list describes them (read by the box over TRS), drawn
// as faders, pickers and toggles, in sections by their control paths, with an envelope editor, an XY pad and wave
// icons where the controls make one (shared/synth/groups.dart; "graphics" turns those off). Moving one sends its CC
// through the box; the synth's own knobs move them back. SynthControls is the controls alone (the desktop's play page
// shows them next to the keys). The values live in SynthState (owned by the box screen), so they are still there the
// next time the page opens.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/synth/controls.dart';
import 'package:airmidi/shared/synth/groups.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/envelope_editor.dart';
import 'package:airmidi/shared/ui/strips_page.dart';
import 'package:airmidi/shared/ui/wave_icon.dart';
import 'package:airmidi/shared/ui/xy_pad.dart';

/// The app bar's buttons for a synth: graphics on/off (to compare the page with and without its widgets), rescan.
List<Widget> synthActions(SynthState synth) => [
      ListenableBuilder(
        listenable: synth,
        builder: (context, _) => Row(mainAxisSize: MainAxisSize.min, children: [
          TextButton(
            onPressed: () => synth.widgets = !synth.widgets,
            child: Text('graphics', style: TextStyle(color: synth.widgets ? cAc : cGrey)),
          ),
          TextButton(
              onPressed: synth.scanning ? null : synth.rescan, child: Text(synth.scanning ? 'scanning' : 'rescan')),
        ]),
      ),
    ];

/// The phone's synth page: the controls, one column.
class SynthPage extends StatelessWidget {
  final SynthState synth;
  final MidiLink link;
  const SynthPage({super.key, required this.synth, required this.link});

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(
          title: ListenableBuilder(
              listenable: synth,
              builder: (context, _) {
                final d = synth.device;
                return Text(d == null || d.name.isEmpty ? 'synth' : d.name);
              }),
          backgroundColor: cBg,
          actions: synthActions(synth),
        ),
        body: SynthControls(synth: synth, link: link),
      );
}

/// Which of [n] columns each block of [heights] goes in: in order, a column takes blocks up to [height] (or the share
/// of the total that makes them all fit in [n], whichever is more); the last column takes what is left.
List<int> fillColumns(List<double> heights, int n, double height) {
  final total = heights.fold(0.0, (a, b) => a + b);
  var target = math.max(height, total / n);
  while (true) {
    final out = <int>[];
    var col = 0, used = 0.0;
    for (final h in heights) {
      if (used > 0 && used + h > target && col < n - 1) {
        col++;
        used = 0;
      }
      out.add(col);
      used += h;
    }
    if (used <= target || target >= total) return out;
    target += 20; // the last column ran over: a little more in each
  }
}

/// The synth's controls, scrolling. [column] set (the desktop): columns at least that wide, as many as fit, filled in
/// order; null: one under the other.
class SynthControls extends StatefulWidget {
  final SynthState synth;
  final MidiLink link;
  final double? column;
  final EdgeInsets padding;
  const SynthControls(
      {super.key, required this.synth, required this.link, this.column, this.padding = const EdgeInsets.all(14)});

  @override
  State<SynthControls> createState() => _SynthControlsState();
}

class _SynthControlsState extends State<SynthControls> {
  final dirty = <Control>{};
  Timer? sendTimer;

  Device? get device => widget.synth.device;

  String? get message {
    final s = widget.synth;
    if (s.scanMessage != null) return s.scanMessage;
    if (s.error != null) return s.error;
    if (device != null && device!.controls.isEmpty) {
      return 'No controls: the box has not found a MIDI-CI device on its TRS jacks. Check the cables and rescan.';
    }
    return null;
  }

  void _changed() {
    if (mounted) setState(() {});
  }

  @override
  void initState() {
    super.initState();
    widget.synth.addListener(_changed);
    widget.synth.attach(widget.link);
    widget.synth.load().then((_) => widget.synth.askCurrent()); // where the knobs are now
  }

  @override
  void dispose() {
    widget.synth.removeListener(_changed);
    sendTimer?.cancel();
    _flush();
    super.dispose();
  }

  // A drag moves a value many times a frame; what goes out is the newest value per control every 15 ms.
  void _set(Control c, int v) {
    v = v.clamp(c.lo, c.hi);
    if (v == c.value) return;
    setState(() => c.value = v);
    dirty.add(c);
    sendTimer ??= Timer(const Duration(milliseconds: 15), () {
      sendTimer = null;
      _flush();
    });
  }

  void _flush() {
    if (dirty.isEmpty) return;
    final msgs = [for (final c in dirty) Device.message(c)];
    dirty.clear();
    widget.link.send(msgs).catchError((_) {});
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(builder: (context, box) {
        final d = device, col = widget.column, pad = widget.padding;
        final secs = d == null ? <Section>[] : sections(d, widgets: widget.synth.widgets);
        final w = box.maxWidth - pad.horizontal, h = box.maxHeight - pad.vertical - 60; // the notes below
        final n = col == null ? 1 : math.max(1, ((w + _gap) / (col + _gap)).floor());
        return ListView(padding: pad, children: [
          if (message != null)
            Padding(
                padding: const EdgeInsets.only(bottom: 12), child: Text(message!, style: const TextStyle(color: cN))),
          if (d != null) ...[
            if (n == 1) ..._list(d, secs) else _columns(d, secs, n, (w - (n - 1) * _gap) / n, h),
            if (d.hidden > 0)
              Padding(
                padding: const EdgeInsets.only(top: 14),
                child: Text('${d.hidden} controls not shown (hidden, per-note or not supported yet)',
                    style: const TextStyle(color: cGrey, fontSize: 12)),
              ),
            Padding(
              padding: const EdgeInsets.only(top: 14),
              child: Text(
                  widget.synth.report == 'ok'
                      ? 'Values read from the synth (MIDI-CI MIDI Message Report) and kept in step with its knobs. '
                          'Double-tap a fader or a handle for its default.'
                      : 'This synth does not report its current values (${widget.synth.report ?? 'asking'}): they '
                          'start at its defaults and follow as soon as you or the synth move them. Double-tap a fader '
                          'for its default.',
                  style: const TextStyle(color: cGrey, fontSize: 12)),
            ),
          ],
        ]);
      });

  Widget _heading(String title) => Padding(
        padding: const EdgeInsets.only(top: 6, bottom: 6),
        child: Text(title, style: const TextStyle(color: cGrey, fontSize: 12, letterSpacing: 1.5)),
      );

  // A section's widgets with a rough height each (for the columns): faders, envelopes and XY pads in the list's
  // order, then pickers, then the toggles in one row; the last one carries the gap to the next section.
  List<(double, Widget)> _blocks(Device d, Section s) {
    final controls = s.items.whereType<Control>();
    final toggles = [
      for (final c in controls)
        if (d.kind(c) == Kind.toggle) _toggle(c)
    ];
    final out = <(double, Widget)>[
      for (final i in s.items)
        if (i is Envelope)
          (226.0, EnvelopeEditor(device: d, env: i, onSet: _set))
        else if (i is XyPad)
          (256.0, XyPadEditor(device: d, pad: i, onSet: _set))
        else if (i is Control && d.kind(i) == Kind.fader)
          (60.0, _fader(d, i)),
      for (final c in controls)
        if (d.kind(c) == Kind.select) (82.0, _select(d, c)),
      if (toggles.isNotEmpty)
        (
          52.0,
          Padding(padding: const EdgeInsets.only(bottom: 8), child: Wrap(spacing: 8, runSpacing: 8, children: toggles))
        ),
    ];
    if (out.isEmpty) return out;
    final (h, w) = out.last;
    out.last =
        (h + 10, Column(crossAxisAlignment: CrossAxisAlignment.start, children: [w, const SizedBox(height: 10)]));
    return out;
  }

  // One under the other: each section's heading, then its blocks.
  List<Widget> _list(Device d, List<Section> secs) => [
        for (final s in secs) ...[
          if (s.title.isNotEmpty) _heading(s.title),
          for (final (_, w) in _blocks(d, s)) w,
        ]
      ];

  // Side by side: [n] columns [width] wide, filled in order as a newspaper fills them, about [height] tall each (more
  // when it all does not fit). A section that goes on in the next column has its heading again at the top.
  Widget _columns(Device d, List<Section> secs, int n, double width, double height) {
    final blocks = <(String, bool, double, Widget)>[
      for (final s in secs)
        for (final (i, (h, w)) in _blocks(d, s).indexed)
          (s.title, i == 0, h + (i == 0 && s.title.isNotEmpty ? 26 : 0), w)
    ];
    final at = fillColumns([for (final b in blocks) b.$3], n, height);
    final cols = List.generate(n, (_) => <Widget>[]);
    for (var k = 0; k < blocks.length; k++) {
      final (title, first, _, w) = blocks[k];
      final col = cols[at[k]];
      if (title.isNotEmpty && (first || col.isEmpty)) col.add(_heading(title));
      col.add(w);
    }
    return Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
      for (var c = 0; c < n; c++) ...[
        if (c > 0) const SizedBox(width: _gap),
        SizedBox(width: width, child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: cols[c])),
      ]
    ]);
  }

  static const _gap = 24.0;

  // A tile that fills to its value (the forslike tile): drag sideways, the whole width is the whole range.
  Widget _fader(Device d, Control c) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: LayoutBuilder(builder: (context, box) {
          return GestureDetector(
            behavior: HitTestBehavior.opaque,
            onHorizontalDragUpdate: (e) => _set(c, c.value + (e.delta.dx / box.maxWidth * (c.hi - c.lo)).round()),
            onDoubleTap: () => _set(c, c.def),
            onHorizontalDragEnd: (_) {
              if (c.type == 'pBend') _set(c, 0x80000000); // a bend springs back
            },
            child: SizedBox(
              height: 52,
              child: Stack(children: [
                Container(color: stripHues[0].base.withValues(alpha: 0.18)),
                FractionallySizedBox(widthFactor: d.fraction(c), child: Container(color: stripHues[0].base)),
                Positioned.fill(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 12),
                    child: Row(children: [
                      Expanded(child: Text(c.title, style: const TextStyle(color: cN, fontSize: 15))),
                      Text(display(d, c), style: const TextStyle(color: cN, fontSize: 15)),
                    ]),
                  ),
                ),
              ]),
            ),
          );
        }),
      );

  Widget _select(Device d, Control c) {
    final opts = d.options(c), on = d.nearestOption(c);
    final shapes = [for (final o in opts) waveShapeOf(o.title)];
    final icons = widget.synth.widgets && shapes.every((s) => s != null);
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(c.title, style: const TextStyle(color: cN, fontSize: 13)),
        const SizedBox(height: 6),
        Wrap(spacing: 6, runSpacing: 6, children: [
          for (var j = 0; j < opts.length; j++)
            if (icons)
              _iconCell(shapes[j]!, j == on, () => _set(c, opts[j].value))
            else
              _cell(opts[j].title, j == on, () => _set(c, opts[j].value)),
        ]),
      ]),
    );
  }

  Widget _toggle(Control c) {
    final on = c.value >= 0x80000000;
    if (c.typeHint == 'momentary') {
      return GestureDetector(
        onTapDown: (_) => _set(c, kMax32),
        onTapUp: (_) => _set(c, 0),
        onTapCancel: () => _set(c, 0),
        child: _box(c.title, on),
      );
    }
    return GestureDetector(onTap: () => _set(c, on ? 0 : kMax32), child: _box(c.title, on));
  }

  Widget _iconCell(WaveShape shape, bool on, VoidCallback onTap) => GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Container(
          width: 72,
          height: 44,
          decoration: BoxDecoration(
            color: on ? stripHues[2].base : Colors.transparent,
            border: Border.all(color: on ? stripHues[2].base : cGrey),
          ),
          child: Center(child: WaveIcon(shape, color: on ? cBg : cN)),
        ),
      );

  Widget _cell(String label, bool on, VoidCallback onTap) =>
      GestureDetector(behavior: HitTestBehavior.opaque, onTap: onTap, child: _box(label, on, minWidth: 64));

  // As wide as its label (a set alignment would stretch it across the whole row inside a Wrap).
  Widget _box(String label, bool on, {double minWidth = 96}) => Container(
        constraints: BoxConstraints(minWidth: minWidth),
        height: 44,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        decoration: BoxDecoration(
          color: on ? stripHues[2].base : Colors.transparent,
          border: Border.all(color: on ? stripHues[2].base : cGrey),
        ),
        child: Center(widthFactor: 1, child: Text(label, style: TextStyle(color: on ? cBg : cN, fontSize: 14))),
      );
}

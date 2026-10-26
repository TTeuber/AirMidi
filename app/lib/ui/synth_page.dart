// The synth behind the box: its controls as its MIDI-CI control list describes them (read by the box over TRS), drawn
// as faders, pickers and toggles, in sections by their control paths, and an envelope editor where four of them make
// one (lib/synth/groups.dart). Moving one sends its CC through the box; the synth's own knobs move them back. The
// values live in SynthState (owned by the box screen), so they are still there the next time the page opens.
import 'dart:async';

import 'package:flutter/material.dart';

import '../link/link.dart';
import '../synth/controls.dart';
import '../synth/groups.dart';
import '../synth/state.dart';
import 'envelope_editor.dart';
import 'strips_page.dart';

class SynthPage extends StatefulWidget {
  final SynthState synth;
  final MidiLink link;
  const SynthPage({super.key, required this.synth, required this.link});

  @override
  State<SynthPage> createState() => _SynthPageState();
}

class _SynthPageState extends State<SynthPage> {
  String? scanMessage;
  bool scanning = false;
  final dirty = <Control>{};
  Timer? sendTimer;

  Device? get device => widget.synth.device;

  String? get message {
    if (scanMessage != null) return scanMessage;
    if (widget.synth.error != null) return widget.synth.error;
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

  // Ask the box to read the device again (Discovery + Property Exchange over TRS, under a second).
  Future<void> _rescan() async {
    setState(() => (scanning = true, scanMessage = 'asking the device on TRS'));
    try {
      final api = widget.synth.config;
      await api.call('POST', '/api/ci/scan', {'force': true});
      for (var i = 0; i < 40; i++) {
        await Future.delayed(const Duration(milliseconds: 250));
        final s = await api.call('GET', '/api/ci');
        if (s['scanning'] != true) break;
      }
      scanMessage = null;
      await widget.synth.load(force: true);
    } catch (e) {
      scanMessage = '$e';
    } finally {
      if (mounted) setState(() => scanning = false);
    }
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
  Widget build(BuildContext context) {
    final d = device;
    return Scaffold(
      appBar: AppBar(
        title: Text(d == null || d.name.isEmpty ? 'synth' : d.name),
        backgroundColor: cBg,
        actions: [
          TextButton(onPressed: scanning ? null : _rescan, child: Text(scanning ? 'scanning' : 'rescan')),
        ],
      ),
      body: ListView(padding: const EdgeInsets.all(14), children: [
        if (message != null)
          Padding(padding: const EdgeInsets.only(bottom: 12), child: Text(message!, style: const TextStyle(color: cN))),
        if (d != null) ...[
          for (final s in sections(d)) ..._section(d, s),
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
                        'Double-tap a fader for its default.'
                    : 'This synth does not report its current values (${widget.synth.report ?? 'asking'}): they start '
                        'at its defaults and follow as soon as you or the synth move them. Double-tap a fader for '
                        'its default.',
                style: const TextStyle(color: cGrey, fontSize: 12)),
          ),
        ],
      ]),
    );
  }

  // A section: its heading (the first path segment), then faders and envelopes in the list's order, pickers, toggles.
  List<Widget> _section(Device d, Section s) {
    final controls = s.items.whereType<Control>();
    final toggles = [for (final c in controls) if (d.kind(c) == Kind.toggle) _toggle(c)];
    return [
      if (s.title.isNotEmpty)
        Padding(
          padding: const EdgeInsets.only(top: 6, bottom: 6),
          child: Text(s.title, style: const TextStyle(color: cGrey, fontSize: 12, letterSpacing: 1.5)),
        ),
      for (final i in s.items)
        if (i is Envelope)
          EnvelopeEditor(device: d, env: i, onSet: _set)
        else if (i is Control && d.kind(i) == Kind.fader)
          _fader(d, i),
      for (final c in controls)
        if (d.kind(c) == Kind.select) _select(d, c),
      if (toggles.isNotEmpty)
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Wrap(spacing: 8, runSpacing: 8, children: toggles),
        ),
      const SizedBox(height: 10),
    ];
  }

  // A tile that fills to its value (the forslike tile): drag sideways, the whole width is the whole range.
  Widget _fader(Device d, Control c) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: LayoutBuilder(builder: (context, box) {
          return GestureDetector(
            behavior: HitTestBehavior.opaque,
            onHorizontalDragUpdate: (e) =>
                _set(c, c.value + (e.delta.dx / box.maxWidth * (c.hi - c.lo)).round()),
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
                      Text(d.shown(c), style: const TextStyle(color: cN, fontSize: 15)),
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
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(c.title, style: const TextStyle(color: cN, fontSize: 13)),
        const SizedBox(height: 6),
        Wrap(spacing: 6, runSpacing: 6, children: [
          for (var j = 0; j < opts.length; j++) _cell(opts[j].title, j == on, () => _set(c, opts[j].value)),
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

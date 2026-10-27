// The router page (docs/desktop.md "Direction"): the computer's MIDI inputs down the side, its outputs across the
// top; a click on a cell makes a route (or selects it), the panel below edits the selected one: which channels
// pass and where each goes, transpose, a note range, message types. Routes run in the engine (desktop/router.dart).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:midi_engine/midi_engine.dart';

import 'package:airmidi/desktop/desktop_midi.dart';
import 'package:airmidi/desktop/router.dart';
import 'package:airmidi/shared/play/keys.dart';
import 'package:airmidi/shared/ui/theme.dart';

class RouterPage extends StatefulWidget {
  const RouterPage({super.key});

  @override
  State<RouterPage> createState() => _RouterPageState();
}

class _RouterPageState extends State<RouterPage> {
  final router = MidiRouter.instance;
  RouteSpec? sel;
  Timer? lights;
  final lastCount = <RouteSpec, int>{}, active = <RouteSpec>{};

  @override
  void initState() {
    super.initState();
    router.addListener(_changed);
    // a route that passed something in the last 150 ms lights up
    lights = Timer.periodic(const Duration(milliseconds: 150), (_) {
      final now = <RouteSpec>{};
      for (final r in router.routes) {
        final c = router.count(r);
        if (c != (lastCount[r] ?? c)) now.add(r);
        lastCount[r] = c;
      }
      if (now.length != active.length || !now.containsAll(active)) {
        setState(() => active
          ..clear()
          ..addAll(now));
      }
    });
  }

  void _changed() => setState(() {});

  @override
  void dispose() {
    lights?.cancel();
    router.removeListener(_changed);
    super.dispose();
  }

  String _label(String port, {required bool output}) => port == kVirtualName
      ? (output ? '$kVirtualName (to Logic, other apps)' : '$kVirtualName (from Logic, other apps)')
      : port;

  @override
  Widget build(BuildContext context) {
    final from = router.sources, to = router.destinations;
    // routes whose ports are not there now still show, greyed
    for (final r in router.routes) {
      if (!from.contains(r.from)) from.add(r.from);
      if (!to.contains(r.to)) to.add(r.to);
    }
    return Scaffold(
      appBar: AppBar(title: const Text('router'), backgroundColor: cBg),
      body: ListView(padding: const EdgeInsets.all(14), children: [
        const Text('from (down) → to (across). A click makes a route or picks it; the panel below edits it.',
            style: TextStyle(color: cGrey, fontSize: 12)),
        const SizedBox(height: 10),
        SingleChildScrollView(scrollDirection: Axis.horizontal, child: _grid(from, to)),
        const SizedBox(height: 18),
        if (sel != null && router.routes.contains(sel)) _inspector(sel!),
      ]),
    );
  }

  static const _rowH = 40.0, _labelW = 240.0, _cellW = 120.0;

  Widget _grid(List<String> from, List<String> to) => Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          const SizedBox(width: _labelW),
          for (final o in to)
            SizedBox(
              width: _cellW,
              height: 52,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 4),
                child: Align(
                  alignment: Alignment.bottomLeft,
                  child: Text(_label(o, output: true),
                      maxLines: 3, overflow: TextOverflow.ellipsis, style: const TextStyle(color: cN, fontSize: 12)),
                ),
              ),
            ),
        ]),
        for (final i in from)
          Row(children: [
            SizedBox(
              width: _labelW,
              height: _rowH,
              child: Align(
                alignment: Alignment.centerLeft,
                child: Text(_label(i, output: false),
                    overflow: TextOverflow.ellipsis, style: const TextStyle(color: cN, fontSize: 13)),
              ),
            ),
            for (final o in to) _cell(i, o),
          ]),
      ]);

  Widget _cell(String from, String to) {
    final r = router.find(from, to), picked = r != null && r == sel;
    final on = r != null, live = r != null && router.live(r), lit = r != null && active.contains(r);
    return GestureDetector(
      onTap: () => setState(() => sel = r ?? router.add(from, to)),
      child: Container(
        width: _cellW - 4,
        height: _rowH - 4,
        margin: const EdgeInsets.all(2),
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: !on
              ? stripHues[0].base.withValues(alpha: 0.10)
              : lit
                  ? stripHues[0].drag
                  : stripHues[0].base.withValues(alpha: live ? 0.75 : 0.25),
          border: Border.all(color: picked ? cAc : Colors.transparent, width: 2),
        ),
        child: on
            ? Text(r.summary, overflow: TextOverflow.ellipsis, style: TextStyle(color: lit ? cBg : cN, fontSize: 12))
            : null,
      ),
    );
  }

  Widget _inspector(RouteSpec r) {
    void edit(void Function() f) {
      f();
      router.changed(r);
    }

    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        Expanded(
          child: Text(
              '${_label(r.from, output: false)}  →  ${_label(r.to, output: true)}'
              '${router.live(r) ? '' : '   (not connected now)'}',
              style: const TextStyle(color: cAc, fontSize: 14)),
        ),
        _chip(
            'remove',
            false,
            () => setState(() {
                  router.remove(r);
                  sel = null;
                })),
      ]),
      const SizedBox(height: 12),
      Row(children: [
        const Text('channels', style: TextStyle(color: cGrey, fontSize: 12)),
        const SizedBox(width: 12),
        _chip('all', r.mask == 0xFFFF, () => edit(() => r.mask = 0xFFFF)),
        const SizedBox(width: 6),
        _chip('none', r.mask == 0, () => edit(() => r.mask = 0)),
        const SizedBox(width: 6),
        _chip('as they are', List.generate(16, (c) => c).every((c) => r.map[c] == c),
            () => edit(() => r.map.setAll(0, List.generate(16, (c) => c)))),
        const SizedBox(width: 16),
        const Text('all to', style: TextStyle(color: cGrey, fontSize: 12)),
        const SizedBox(width: 6),
        DropdownButton<int>(
          value: null,
          hint: const Text('channel', style: TextStyle(color: cGrey, fontSize: 12)),
          dropdownColor: cBg,
          style: const TextStyle(color: cN, fontSize: 13),
          items: [for (var c = 0; c < 16; c++) DropdownMenuItem(value: c, child: Text('${c + 1}'))],
          onChanged: (v) => v == null ? null : edit(() => r.map.fillRange(0, 16, v)),
        ),
      ]),
      const SizedBox(height: 8),
      // a cell per source channel: click passes or blocks it, the menu says where it goes
      Wrap(spacing: 6, runSpacing: 6, children: [
        for (var c = 0; c < 16; c++)
          Container(
            width: 74,
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 4),
            decoration: BoxDecoration(
              color: r.passes(c) ? stripHues[1].base.withValues(alpha: 0.6) : null,
              border: Border.all(color: r.passes(c) ? stripHues[1].drag : cGrey),
            ),
            child: Column(children: [
              GestureDetector(
                onTap: () => edit(() => r.mask ^= 1 << c),
                child: Text('ch ${c + 1}', style: TextStyle(color: r.passes(c) ? cN : cGrey, fontSize: 13)),
              ),
              DropdownButton<int>(
                value: r.map[c],
                isDense: true,
                underline: const SizedBox.shrink(),
                dropdownColor: cBg,
                style: TextStyle(color: r.passes(c) ? cAc : cGrey, fontSize: 12),
                items: [for (var d = 0; d < 16; d++) DropdownMenuItem(value: d, child: Text('→ ${d + 1}'))],
                onChanged: (v) => v == null ? null : edit(() => r.map[c] = v),
              ),
            ]),
          ),
      ]),
      const SizedBox(height: 14),
      Wrap(spacing: 18, runSpacing: 8, crossAxisAlignment: WrapCrossAlignment.center, children: [
        _stepper('transpose', '${r.transpose}', () => edit(() => r.transpose = (r.transpose - 1).clamp(-48, 48)),
            () => edit(() => r.transpose = (r.transpose + 1).clamp(-48, 48))),
        _stepper('lowest', noteName(r.lo), () => edit(() => r.lo = (r.lo - 1).clamp(0, r.hi)),
            () => edit(() => r.lo = (r.lo + 1).clamp(0, r.hi))),
        _stepper('highest', noteName(r.hi), () => edit(() => r.hi = (r.hi - 1).clamp(r.lo, 127)),
            () => edit(() => r.hi = (r.hi + 1).clamp(r.lo, 127))),
      ]),
      const SizedBox(height: 10),
      Wrap(spacing: 6, runSpacing: 6, crossAxisAlignment: WrapCrossAlignment.center, children: [
        const Text('passes', style: TextStyle(color: cGrey, fontSize: 12)),
        for (final (name, bit) in const [
          ('notes', RouteTypes.notes),
          ('CC', RouteTypes.cc),
          ('program', RouteTypes.program),
          ('pressure', RouteTypes.pressure),
          ('bend', RouteTypes.bend),
          ('clock', RouteTypes.clock),
          ('SysEx', RouteTypes.sysex),
          ('other', RouteTypes.other),
        ])
          _chip(name, r.types & bit != 0, () => edit(() => r.types ^= bit)),
      ]),
      const SizedBox(height: 8),
      const Text(
          'SysEx is off by default: MIDI-CI between two boxes or apps can go round a loop. macOS lets every app hear '
          'every input, so a route adds a destination; it does not hide an input from Logic.',
          style: TextStyle(color: cGrey, fontSize: 12)),
    ]);
  }

  Widget _stepper(String label, String value, VoidCallback down, VoidCallback up) =>
      Row(mainAxisSize: MainAxisSize.min, children: [
        Text(label, style: const TextStyle(color: cGrey, fontSize: 12)),
        IconButton(
            onPressed: down,
            icon: const Icon(Icons.remove, size: 14),
            color: cAc,
            visualDensity: VisualDensity.compact),
        SizedBox(width: 34, child: Center(child: Text(value, style: const TextStyle(color: cN, fontSize: 13)))),
        IconButton(
            onPressed: up, icon: const Icon(Icons.add, size: 14), color: cAc, visualDensity: VisualDensity.compact),
      ]);

  Widget _chip(String label, bool on, VoidCallback tap) => GestureDetector(
        onTap: tap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(color: on ? cAc : null, border: Border.all(color: on ? cAc : cGrey)),
          child: Text(label, style: TextStyle(color: on ? cBg : cN, fontSize: 12)),
        ),
      );
}

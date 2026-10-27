// The sequencer (docs/daw.md "The sequencer"): a pattern clip's rows by steps in the bottom panel. A click toggles a
// step. The pattern's length, step size, swing and rows (a drum map or an octave of notes) are set above the grid.
// Velocity, gate, probability, nudge and repeats per step come in A3.
import 'package:flutter/material.dart';

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/shared/ui/theme.dart';

class PatternEditor extends StatelessWidget {
  final Pattern pattern;
  final History history;
  final VoidCallback changed;
  final int playStep; // the step playing now, -1 when this clip is not playing
  const PatternEditor(
      {super.key, required this.pattern, required this.history, required this.changed, this.playStep = -1});

  void _edit(void Function() f) {
    history.mark();
    f();
    changed();
  }

  @override
  Widget build(BuildContext context) {
    final p = pattern;
    final perBeat = (1 / p.stepBeats).round();
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Wrap(spacing: 16, runSpacing: 6, crossAxisAlignment: WrapCrossAlignment.center, children: [
        stepper('steps', '${p.steps}', () => _edit(() => p.steps = (p.steps - 1).clamp(1, kMaxSteps)),
            () => _edit(() => p.steps = (p.steps + 1).clamp(1, kMaxSteps))),
        Row(mainAxisSize: MainAxisSize.min, children: [
          const Text('step ', style: TextStyle(color: cGrey, fontSize: 12)),
          for (final (label, b) in const [('1/8', 0.5), ('1/16', 0.25), ('1/32', 0.125)])
            Padding(
                padding: const EdgeInsets.only(right: 4),
                child: chip(label, p.stepBeats == b, () => _edit(() => p.stepBeats = b))),
        ]),
        stepper('swing', '${(p.swing * 100).round()}%', () => _edit(() => p.swing = (p.swing - 0.02).clamp(0.5, 0.75)),
            () => _edit(() => p.swing = (p.swing + 0.02).clamp(0.5, 0.75))),
        Row(mainAxisSize: MainAxisSize.min, children: [
          const Text('rows ', style: TextStyle(color: cGrey, fontSize: 12)),
          chip('drums', false, () => _edit(() => _rows(gmDrumRows()))),
          const SizedBox(width: 4),
          chip('notes c2', false, () => _edit(() => _rows(chromaticRows(36)))),
          const SizedBox(width: 4),
          chip('notes c3', false, () => _edit(() => _rows(chromaticRows(48)))),
        ]),
      ]),
      const SizedBox(height: 8),
      Expanded(
        child: LayoutBuilder(builder: (context, box) {
          const labelW = 80.0;
          final cell = ((box.maxWidth - labelW) / p.steps - 1).clamp(12.0, 34.0);
          final rowH = ((box.maxHeight / p.rows.length) - 1).clamp(14.0, 26.0);
          return SingleChildScrollView(
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                for (var r = 0; r < p.rows.length; r++)
                  Row(children: [
                    SizedBox(
                      width: labelW,
                      child: GestureDetector(
                        onTap: () => _edit(() => p.rows[r].mute = !p.rows[r].mute),
                        child: Text(p.rows[r].name,
                            style: TextStyle(color: p.rows[r].mute ? cGrey : cN, fontSize: 12),
                            overflow: TextOverflow.ellipsis),
                      ),
                    ),
                    for (var s = 0; s < p.steps; s++) _cell(r, s, cell, rowH, s % perBeat == 0),
                  ]),
              ]),
            ),
          );
        }),
      ),
    ]);
  }

  // Rows from a set; steps on rows of the same note stay
  void _rows(List<PatternRow> rows) {
    final old = {for (var r = 0; r < pattern.rows.length; r++) pattern.rows[r].note: pattern.cells[r]};
    while (pattern.rows.isNotEmpty) {
      pattern.removeRow(0);
    }
    for (final r in rows) {
      pattern.addRow(r);
      final keep = old[r.note];
      if (keep != null) pattern.cells.last.setAll(0, keep);
    }
  }

  Widget _cell(int r, int s, double w, double h, bool beatStart) {
    final on = pattern.cells[r][s] != null, here = s == playStep;
    final hue = stripHues[(s * pattern.stepBeats).floor() % 3];
    return GestureDetector(
      onTap: () => _edit(() => pattern.toggle(r, s)),
      child: Container(
        width: w,
        height: h,
        margin: EdgeInsets.only(left: beatStart && s > 0 ? 3 : 1, bottom: 1),
        color: on
            ? (here ? cAc : hue.drag)
            : (here ? cAc.withValues(alpha: 0.25) : hue.base.withValues(alpha: beatStart ? 0.28 : 0.16)),
      ),
    );
  }
}

Widget stepper(String label, String value, VoidCallback down, VoidCallback up) =>
    Row(mainAxisSize: MainAxisSize.min, children: [
      Text(label, style: const TextStyle(color: cGrey, fontSize: 12)),
      IconButton(
          onPressed: down, icon: const Icon(Icons.remove, size: 14), color: cAc, visualDensity: VisualDensity.compact),
      Text(value, style: const TextStyle(color: cN, fontSize: 13)),
      IconButton(
          onPressed: up, icon: const Icon(Icons.add, size: 14), color: cAc, visualDensity: VisualDensity.compact),
    ]);

Widget chip(String label, bool on, VoidCallback? tap) => GestureDetector(
      onTap: tap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 5),
        decoration: BoxDecoration(color: on ? cAc : null, border: Border.all(color: on ? cAc : cGrey)),
        child: Text(label, style: TextStyle(color: on ? cBg : (tap == null ? cGrey : cN), fontSize: 12)),
      ),
    );

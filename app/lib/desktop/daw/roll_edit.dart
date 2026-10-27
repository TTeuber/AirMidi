// The piano roll's edits (docs/daw.md "The piano roll"), as plain functions on notes so they can be tested without
// a screen: move, resize, quantize, legato, transpose, velocity, box selection, copy and paste. The editor
// (piano_roll.dart) calls them and draws the result.
import 'dart:math' as math;

import 'package:airmidi/desktop/daw/model.dart';

/// Grids the piano roll snaps to, in beats, with their names; 0 is off. Triplets: three in the time of two.
const kRollGrids = <(String, double)>[
  ('1/4', 1),
  ('1/8', 0.5),
  ('1/16', 0.25),
  ('1/32', 0.125),
  ('1/8T', 1 / 3),
  ('1/16T', 1 / 6),
  ('off', 0),
];

double snapTo(double beat, double grid) => grid <= 0 ? beat : (beat / grid).round() * grid;

/// Move [notes] by [dt] beats and [dp] semitones; the group keeps its shape at the edges (no note before 0, all
/// pitches inside 0..127).
void moveNotes(Iterable<Note> notes, double dt, int dp) {
  if (notes.isEmpty) return;
  final first = notes.map((n) => n.start).reduce(math.min);
  final lo = notes.map((n) => n.pitch).reduce(math.min), hi = notes.map((n) => n.pitch).reduce(math.max);
  dt = math.max(dt, -first);
  dp = dp.clamp(-lo, 127 - hi);
  for (final n in notes) {
    n.start += dt;
    n.pitch += dp;
  }
}

/// Lengthen or shorten [notes] by [d] beats each, never below [min].
void resizeNotes(Iterable<Note> notes, double d, {double min = 1 / 64}) {
  for (final n in notes) {
    n.length = math.max(min, n.length + d);
  }
}

/// Move each note's start by [d] beats keeping its end (the left edge), within 0 and its end.
void trimStarts(Iterable<Note> notes, double d, {double min = 1 / 64}) {
  for (final n in notes) {
    final end = n.end;
    n.start = (n.start + d).clamp(0.0, end - min);
    n.length = end - n.start;
  }
}

/// Starts towards the grid by [strength] (1 = onto it), each note keeping its length; with [ends], ends too (a
/// note whose ends meet then gets one grid step).
void quantize(Iterable<Note> notes, double grid, {double strength = 1, bool ends = false}) {
  if (grid <= 0) return;
  for (final n in notes) {
    final s = n.start + (snapTo(n.start, grid) - n.start) * strength;
    var e = s + n.length;
    if (ends) {
      e = n.end + (snapTo(n.end, grid) - n.end) * strength;
      if (e <= s + 1e-9) e = s + grid;
    }
    n.start = s;
    n.length = math.max(e - s, 1 / 64);
  }
}

/// Each note runs to the start of the next later note among [notes] (chords stay chords: notes that start
/// together all run to the next start after them).
void legato(Iterable<Note> notes) {
  final starts = notes.map((n) => n.start).toSet().toList()..sort();
  for (final n in notes) {
    final next = starts.where((s) => s > n.start + 1e-9);
    if (next.isNotEmpty) n.length = next.first - n.start;
  }
}

void transpose(Iterable<Note> notes, int d) => moveNotes(notes, 0, d);

/// Velocity (7-bit units) moved by [d] for every note, inside 1..127.
void nudgeVelocity(Iterable<Note> notes, int d) {
  for (final n in notes) {
    n.velocity = up7to16((down16to7(n.velocity) + d).clamp(1, 127));
  }
}

/// The notes that a box from (beat a, pitch p) to (beat b, pitch q) touches.
Set<Note> notesInBox(List<Note> notes, double a, double b, int p, int q) {
  final t0 = math.min(a, b), t1 = math.max(a, b), lo = math.min(p, q), hi = math.max(p, q);
  return {
    for (final n in notes)
      if (n.end > t0 && n.start < t1 && n.pitch >= lo && n.pitch <= hi) n
  };
}

/// Copies of [notes] placed so the earliest starts at [at].
List<Note> pasted(List<Note> notes, double at) {
  if (notes.isEmpty) return [];
  final first = notes.map((n) => n.start).reduce(math.min);
  return [for (final n in notes) n.copy()..start = n.start - first + at];
}

/// Copies of [notes] right after themselves (cmd-D): shifted by the span from the first start to the last end,
/// rounded up to [grid].
List<Note> duplicated(List<Note> notes, double grid) {
  if (notes.isEmpty) return [];
  final first = notes.map((n) => n.start).reduce(math.min), last = notes.map((n) => n.end).reduce(math.max);
  var span = last - first;
  if (grid > 0) span = (span / grid - 1e-9).ceil() * grid;
  return [for (final n in notes) n.copy()..start = n.start + span];
}

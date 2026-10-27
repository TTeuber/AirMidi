// The computer keyboard as a piano, and pads (the desktop's ways to play, docs/desktop.md D2). Pure logic: key
// presses and clicks in, MIDI 1.0 messages out; the page (ui/keys_page.dart) only draws and forwards events.
//
// The layout is the one most DAWs use (Logic's Musical Typing, Ableton's computer MIDI keyboard): the home row is
// the white keys from C, the row above the black keys between them. Z/X move an octave, C/V change the velocity,
// Tab holds the sustain pedal. Keys are matched by position (physical key), so other keyboard layouts get the same
// shape.
import 'dart:math' as math;

import 'package:flutter/services.dart';

import 'package:airmidi/shared/strips/strips.dart' show PinchStep;

/// Semitones above the octave's C, by physical key.
final kKeyNotes = <PhysicalKeyboardKey, int>{
  PhysicalKeyboardKey.keyA: 0,
  PhysicalKeyboardKey.keyW: 1,
  PhysicalKeyboardKey.keyS: 2,
  PhysicalKeyboardKey.keyE: 3,
  PhysicalKeyboardKey.keyD: 4,
  PhysicalKeyboardKey.keyF: 5,
  PhysicalKeyboardKey.keyT: 6,
  PhysicalKeyboardKey.keyG: 7,
  PhysicalKeyboardKey.keyY: 8,
  PhysicalKeyboardKey.keyH: 9,
  PhysicalKeyboardKey.keyU: 10,
  PhysicalKeyboardKey.keyJ: 11,
  PhysicalKeyboardKey.keyK: 12,
  PhysicalKeyboardKey.keyO: 13,
  PhysicalKeyboardKey.keyL: 14,
  PhysicalKeyboardKey.keyP: 15,
  PhysicalKeyboardKey.semicolon: 16,
  PhysicalKeyboardKey.quote: 17,
};

/// The letter printed on each key's piano key, by semitone above the octave's C.
const kKeyLabels = ['A', 'W', 'S', 'E', 'D', 'F', 'T', 'G', 'Y', 'H', 'U', 'J', 'K', 'O', 'L', 'P', ';', '\''];

const _names = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b'];

/// "c4" for 60, as on the strips.
String noteName(int n) => '${_names[n % 12]}${n ~/ 12 - 1}';

bool isBlack(int n) => const [1, 3, 6, 8, 10].contains(n % 12);

class Keys {
  int octave = 4; // the home row's A is c<octave>: c4 = 60
  int velocity = 100;
  int channel = 0; // 0-based
  bool sustain = false;

  /// Notes sounding, by what started each (a key, a click on the piano, a pad), with their channel. A note keeps
  /// the number and channel it started with, so changing the octave or channel while it sounds still ends it.
  final held = <Object, (int, int)>{};

  int get base => 12 * (octave + 1);

  /// A key went down: the messages to send (none for keys that do nothing).
  List<List<int>> keyDown(PhysicalKeyboardKey k) {
    final semis = kKeyNotes[k];
    if (semis != null) return noteOn(k, base + semis);
    if (k == PhysicalKeyboardKey.keyZ) octave = (octave - 1).clamp(-1, 8);
    if (k == PhysicalKeyboardKey.keyX) octave = (octave + 1).clamp(-1, 8);
    if (k == PhysicalKeyboardKey.keyC) velocity = (velocity - 20).clamp(1, 127);
    if (k == PhysicalKeyboardKey.keyV) velocity = (velocity + 20).clamp(1, 127);
    if (k == PhysicalKeyboardKey.tab) return pedal(true);
    return [];
  }

  List<List<int>> keyUp(PhysicalKeyboardKey k) {
    if (k == PhysicalKeyboardKey.tab) return pedal(false);
    return noteOff(k);
  }

  /// A note from [source] (a key, the mouse on the piano, a pad): one note per source, the newest wins.
  List<List<int>> noteOn(Object source, int note, {int? vel, int? ch}) {
    if (note < 0 || note > 127) return [];
    final c = ch ?? channel;
    final out = noteOff(source);
    held[source] = (note, c);
    out.add([0x90 | c, note, (vel ?? velocity).clamp(1, 127)]);
    return out;
  }

  List<List<int>> noteOff(Object source) {
    final n = held.remove(source);
    if (n == null) return [];
    // another source may still hold the same note (a key and a click): it keeps sounding
    if (held.containsValue(n)) return [];
    return [
      [0x80 | n.$2, n.$1, 0]
    ];
  }

  List<List<int>> pedal(bool down) {
    if (sustain == down) return [];
    sustain = down;
    return [
      [0xB0 | channel, 64, down ? 127 : 0]
    ];
  }

  /// Everything off: when the page closes or the window loses the keyboard (a key released elsewhere never comes).
  List<List<int>> allOff() {
    final out = <List<int>>[];
    for (final s in held.keys.toList()) {
      out.addAll(noteOff(s));
    }
    out.addAll(pedal(false));
    return out;
  }

  /// The notes sounding on the keyboard's channel (what the piano lights).
  Set<int> get sounding => {
        for (final (n, c) in held.values)
          if (c == channel) n
      };
}

/// 4 x 4 pads on the General MIDI drum map, bottom left = 36 (kick), as on most pad controllers.
class Pads {
  int channel = 9; // MIDI channel 10, drums
  int first = 36;

  /// The note of the pad in [row] (0 = top) and [col].
  int note(int row, int col) => first + (3 - row) * 4 + col;

  /// The General MIDI drum on [note], for the pad's label ('' off the map).
  static String drum(int note) =>
      const {
        35: 'kick 2',
        36: 'kick',
        37: 'rim',
        38: 'snare',
        39: 'clap',
        40: 'snare 2',
        41: 'low tom',
        42: 'hat',
        43: 'low tom 2',
        44: 'pedal hat',
        45: 'mid tom',
        46: 'open hat',
        47: 'mid tom 2',
        48: 'high tom',
        49: 'crash',
        50: 'high tom 2',
        51: 'ride',
        52: 'china',
        53: 'ride bell',
        54: 'tambourine',
        55: 'splash',
        56: 'cowbell',
        57: 'crash 2',
      }[note] ??
      '';

  /// A hit at [y] (0 = top of the pad, 1 = bottom): loud at the top, soft at the bottom.
  static int velocityAt(double y) => (127 - y.clamp(0.0, 1.0) * 97).round();
}

/// Pitch bend from -1..1 (springs back to 0 on release), as a 14-bit message.
List<int> bendAt(int ch, double v) {
  final b = (8192 + v.clamp(-1.0, 1.0) * 8191).round().clamp(0, 16383);
  return [0xE0 | ch, b & 0x7F, b >> 7];
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

/// The white key [n] is on, counted from note 0 (a black key: the white key below it). 75 white keys in 0..127.
int whiteIndex(int n) => n ~/ 12 * 7 + const [0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6][n % 12];
const kWhiteKeys = 75;

/// The note of white key [i].
int whiteNote(int i) => i ~/ 7 * 12 + const [0, 2, 4, 5, 7, 9, 11][i % 7];

/// The phone's keys (landscape): one keyboard across a [w] x [h] box, scrolled [view] white keys up from note 0, each
/// white key [ww] px wide; black keys on top over the top 60 %, as on [PianoGeometry].
class PianoView {
  final double view, ww, w, h;
  PianoView(this.view, this.ww, this.w, this.h);

  double xOf(double white) => (white - view) * ww;

  /// The notes at least partly on screen.
  int get lo => whiteNote(view.floor().clamp(0, kWhiteKeys - 1));
  int get hi => math.min(127, whiteNote(((view + w / ww).ceil() - 1).clamp(0, kWhiteKeys - 1)) + 1);

  Rect rect(int n) => isBlack(n)
      ? Rect.fromLTWH(xOf(whiteIndex(n - 1).toDouble()) + ww * 0.68, 0, ww * 0.64, h * 0.6)
      : Rect.fromLTWH(xOf(whiteIndex(n).toDouble()), 0, ww, h);

  /// The note under (x, y) and how hard it was struck: harder towards the front (bottom) of the key, 40..127.
  (int, int)? hit(double x, double y) {
    if (x < 0 || x >= w || y < 0 || y >= h) return null;
    int? n;
    for (var b = lo; b <= hi; b++) {
      if (isBlack(b) && rect(b).contains(Offset(x, y))) n = b;
    }
    if (n == null) {
      final i = (view + x / ww).floor();
      if (i < 0 || i >= kWhiteKeys) return null;
      n = whiteNote(i);
    }
    final k = rect(n);
    return (n, (40 + 87 * ((y - k.top) / k.height).clamp(0.0, 1.0)).round());
  }
}

const kKeyWidthRange = (7.0, 30.0); // mm, a white key

/// Move mode on the phone's keys: one finger scrolls, two pinched across change the key width round their centre
/// (the key under it stays under it). Scroll ends on the keyboard's ends. Returns (view, white key width in mm).
(double, double) movedKeys(double view, double widthMm, double w, double pxPerMm, PinchStep p) {
  final ww0 = widthMm * pxPerMm;
  final width = (widthMm * p.sx).clamp(kKeyWidthRange.$1, kKeyWidthRange.$2), ww1 = width * pxPerMm;
  final v = view + p.fx / ww0 - p.nx / ww1;
  return (v.clamp(0.0, math.max(0.0, kWhiteKeys - w / ww1)), width);
}

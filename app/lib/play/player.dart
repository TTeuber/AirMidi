// Strip events -> MpeOut -> the link, one batch in flight at a time, with the numbers the A0 spike is after.
import 'dart:async';

import '../link/link.dart';
import '../midi/mpe.dart';
import '../strips/strips.dart';

class SendStats {
  int batches = 0, messages = 0;
  double sendMsMax = 0, sendMsSum = 0; // time the link took to take a batch
  double waitMsMax = 0, waitMsSum = 0; // oldest due event -> its batch handed to the link
  int errors = 0;

  double get sendMsMean => batches == 0 ? 0 : sendMsSum / batches;
  double get waitMsMean => batches == 0 ? 0 : waitMsSum / batches;
}

/// At least this long between batches: Android takes a write long before the radio sends it, so the link alone
/// would not pace the loop. 8 ms is about one BLE connection event, and three strips' bends every 8 ms (~1100
/// bytes/s) stay well inside what the box's TRS output carries (3125 bytes/s).
const kMinBatchGap = Duration(milliseconds: 8);

/// The MPE zone setup goes out again this often while nothing sounds: a synth that was reset (back to one ±2
/// bend for all channels) is in MPE again within seconds instead of bending a 24th as far.
const kSetupRepeat = Duration(seconds: 3);

class Player {
  MidiLink? link;
  int _strips = 0;
  final MpeOut mpe;
  final _clock = Stopwatch()..start();
  bool _busy = false;
  double? _dueSince; // ms, when the oldest not-yet-sent event arrived
  var stats = SendStats();
  var _window = SendStats(); // the last second, for the overlay
  SendStats shown = SendStats();
  Timer? _tick, _setupTimer;
  String? lastError;

  Player({required bool Function() legato}) : mpe = MpeOut(legato: legato) {
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      shown = _window;
      _window = SendStats();
    });
  }

  Future<void> attach(MidiLink l, int strips) async {
    link = l;
    _strips = strips;
    await l.send(mpeSetup(strips));
    _setupTimer ??= Timer.periodic(kSetupRepeat, (_) {
      final l = link;
      if (l != null && !_busy && !mpe.sounding && !mpe.hasDue) l.send(mpeSetup(_strips)).catchError((_) {});
    });
  }

  void handle(StripEvent? e) {
    if (e == null) return;
    mpe.handle(e);
    _dueSince ??= _clock.elapsedMicroseconds / 1000;
    _kick();
  }

  void handleAll(Iterable<StripEvent> es) => es.forEach(handle);

  /// Note off for what the app knows is sounding, then All Notes Off on the manager and every member channel, for
  /// anything the synth still holds that the app does not know about.
  Future<void> allOff() async {
    final msgs = [
      ...mpe.allOff(),
      for (var ch = 0; ch <= _strips; ch++) [0xB0 | ch, 123, 0],
    ];
    await link?.send(msgs);
  }

  Future<void> _kick() async {
    final l = link;
    if (_busy || l == null || !mpe.hasDue) {
      if (l == null) {
        mpe.take(); // no box: play silently
        _dueSince = null;
      }
      return;
    }
    _busy = true;
    while (mpe.hasDue) {
      final now = _clock.elapsedMicroseconds / 1000;
      final wait = now - (_dueSince ?? now);
      _dueSince = null;
      final msgs = mpe.take();
      try {
        await l.send(msgs);
      } catch (e) {
        lastError = '$e';
        stats.errors++;
        _window.errors++;
      }
      final took = _clock.elapsedMicroseconds / 1000 - now;
      for (final s in [stats, _window]) {
        s.batches++;
        s.messages += msgs.length;
        s.sendMsSum += took;
        s.waitMsSum += wait;
        if (took > s.sendMsMax) s.sendMsMax = took;
        if (wait > s.waitMsMax) s.waitMsMax = wait;
      }
      if (mpe.hasDue) _dueSince ??= _clock.elapsedMicroseconds / 1000;
      final rest = kMinBatchGap.inMicroseconds - (_clock.elapsedMicroseconds - now * 1000).round();
      if (rest > 0) await Future.delayed(Duration(microseconds: rest));
    }
    _busy = false;
  }

  Future<void> dispose() async {
    _tick?.cancel();
    _setupTimer?.cancel();
    await allOff();
    await link?.close();
  }
}

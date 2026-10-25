// The device's controls for as long as the box screen is open, not just while the synth page is: the page shows what
// was last sent or heard, and the device's own CCs keep moving the values while the page is closed. Where the knobs
// really are comes from a MIDI-CI MIDI Message Report (askCurrent): the device answers with ordinary CCs and pitch
// bend, which arrive like its knob moves.
import 'dart:async';

import 'package:flutter/foundation.dart';

import '../link/config.dart';
import '../link/link.dart';
import 'controls.dart';

class SynthState extends ChangeNotifier {
  final ConfigClient config;
  Device? device;
  String? error;
  MidiLink? _link;
  StreamSubscription<List<int>>? _sub;

  SynthState(this.config);

  /// The control list, read once; [force] reads it again (after a rescan) and keeps the values of the controls that
  /// are still there.
  Future<void> load({bool force = false}) async {
    if (device != null && !force) return;
    try {
      final fresh = Device.fromApi(await config.call('GET', '/api/ci/device'));
      final old = device;
      if (old != null) {
        for (final c in fresh.controls) {
          for (final o in old.controls) {
            if (o.type == c.type && o.channel == c.channel && listEquals(o.index, c.index)) c.value = o.value;
          }
        }
      }
      device = fresh;
      error = null;
    } catch (e) {
      error = '$e';
    }
    notifyListeners();
  }

  /// Ask the box to ask the device for its current values (MIDI-CI MIDI Message Report). They arrive on the link.
  /// Older box firmware has no such endpoint, and devices without Process Inquiry say so: the values then stay
  /// where they are (the defaults, or what was set here).
  String? report; // the last report's outcome, for the page: "ok", "not supported", ...
  Future<void> askCurrent() async {
    try {
      await config.call('POST', '/api/ci/report');
      for (var i = 0; i < 20; i++) {
        await Future.delayed(const Duration(milliseconds: 150));
        final s = await config.call('GET', '/api/ci');
        final r = s['report'] as String? ?? '';
        if (r != 'asking' && r != '') {
          report = r;
          break;
        }
      }
    } catch (e) {
      report = 'not available';
    }
    notifyListeners();
  }

  /// Follow what arrives on [link] (the strips page's link; a new one replaces the old).
  void attach(MidiLink link) {
    if (identical(link, _link)) return;
    _sub?.cancel();
    _link = link;
    _sub = link.incoming.listen((m) {
      if (device?.receive(m) == true) notifyListeners();
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }
}

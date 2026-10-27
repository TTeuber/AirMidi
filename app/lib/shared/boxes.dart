// Opening a box's page, the same in both apps: over BLE or Wi-Fi, following the box across a mode change.
import 'package:flutter/widgets.dart';

import 'package:airmidi/shared/discovery.dart';
import 'package:airmidi/shared/link/config.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/box_page.dart';

/// Opens a box's page over BLE or Wi-Fi, and follows the box to the other one when a mode change moves it there.
class Boxes {
  final Discovery discovery;
  final Widget Function(MidiLink link, SynthState synth) play; // each app's way to play a box
  final String playLabel;
  Boxes(this.discovery, {required this.play, required this.playLabel});

  Future<Widget> openBle(String deviceId, String name) async {
    await discovery.stopBle();
    try {
      final box = await BleBox.connect(deviceId, name);
      return BoxPage(
        title: box.name,
        overBle: true,
        config: box.config,
        midi: () async => box.midi,
        close: box.close,
        follow: follow,
        play: play,
        playLabel: playLabel,
      );
    } finally {
      discovery.startBle();
    }
  }

  Future<Widget> openWifi(String host, String title) async {
    final config = HttpConfigClient(host);
    await config.call('GET', '/api/status'); // there, and answering
    return BoxPage(
      title: title,
      overBle: false,
      config: config,
      midi: () => WsMidiLink.connect(host),
      close: config.close,
      follow: follow,
      play: play,
      playLabel: playLabel,
    );
  }

  /// The box [id] went away from the transport we used at [since] (a mode change, a restart): find it again, on
  /// Wi-Fi if it went to Network mode, over BLE otherwise. Null if it did not come back within ~40 s.
  Future<Widget?> follow(String id, {required bool toWifi, required DateTime since}) async {
    final end = DateTime.now().add(const Duration(seconds: 40));
    final s = await discovery.waitFor(id, ble: !toWifi, since: since);
    if (s == null) return null;
    // The sighting can come before the box answers (mDNS keeps a box that restarted on the same address; BLE
    // advertises before everything has started): try until it does.
    while (true) {
      try {
        return toWifi ? await openWifi(s.host!, s.name) : await openBle(s.bleDevice!, s.name);
      } catch (_) {
        if (DateTime.now().isAfter(end)) return null;
        await Future.delayed(const Duration(seconds: 1));
      }
    }
  }
}

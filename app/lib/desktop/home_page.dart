// The desktop's first page (docs/desktop.md): boxes (found over BLE and on Wi-Fi, set up as on the phone), the
// computer's MIDI ports to play, and the tracks.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:midi_engine/midi_engine.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:airmidi/desktop/desktop_midi.dart';
import 'package:airmidi/desktop/keys_page.dart';
import 'package:airmidi/desktop/tracks_page.dart';
import 'package:airmidi/shared/boxes.dart';
import 'package:airmidi/shared/discovery.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/ui/theme.dart';

class DesktopHome extends StatefulWidget {
  const DesktopHome({super.key});

  @override
  State<DesktopHome> createState() => _DesktopHomeState();
}

class _DesktopHomeState extends State<DesktopHome> {
  final discovery = Discovery();
  // the desktop has no touch screen: the keys first, the strips a button away
  late final boxes =
      Boxes(discovery, playLabel: 'play', play: (link, synth) => KeysPage(link: link, synth: synth, closeLink: true));
  final host = TextEditingController();
  String? error; // the last connect failure, kept until the next attempt
  bool busy = false;
  StreamSubscription<void>? _portChanges;
  List<MidiPortInfo> ports = []; // the computer's MIDI outputs (packages/midi_engine)

  @override
  void initState() {
    super.initState();
    discovery.addListener(_changed);
    discovery.start();
    SharedPreferences.getInstance().then((p) => host.text = p.getString('host') ?? '');
    try {
      DesktopMidi.virtualPorts(); // made at start, so other apps see them whether or not anything plays
      ports = DesktopMidi.outputs();
      _portChanges = DesktopMidi.changes.listen((_) => setState(() => ports = DesktopMidi.outputs()));
    } catch (e) {
      error = 'MIDI: $e';
    }
  }

  void _changed() => setState(() {});

  @override
  void dispose() {
    _portChanges?.cancel();
    discovery.removeListener(_changed);
    discovery.dispose();
    super.dispose();
  }

  // Straight to the keys page, no box page: a MIDI port has no box API behind it. The link closes with the page.
  Future<void> _play(Future<MidiLink> Function() open) async {
    if (busy) return;
    setState(() => (busy = true, error = null));
    MidiLink? link;
    try {
      link = await open();
    } catch (e) {
      setState(() => error = '$e');
    } finally {
      if (mounted) setState(() => busy = false);
    }
    final l = link;
    if (l == null || !mounted) return;
    await Navigator.of(context).push(MaterialPageRoute(builder: (_) => KeysPage(link: l)));
    await l.close().catchError((_) {});
  }

  Future<void> _open(Future<Widget> Function() page) async {
    if (busy) return;
    setState(() => (busy = true, error = null));
    try {
      final p = await page();
      if (!mounted) return;
      setState(() => busy = false);
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => p));
    } catch (e) {
      setState(() => error = '$e');
    } finally {
      if (mounted) setState(() => busy = false);
    }
  }

  // The desktop: the computer's MIDI outputs, and the app's own virtual port for Logic and other apps.
  List<Widget> _desktopPorts() {
    String kind(MidiPortInfo p) => [
          if (p.isUsb) 'usb',
          if (p.isBluetooth) 'bluetooth',
          if (p.isNetwork) 'network',
          if (p.isVirtual) 'app',
        ].join(' · ');
    return [
      const Text('MIDI ports', style: TextStyle(color: cAc, fontSize: 13)),
      ListTile(
        contentPadding: EdgeInsets.zero,
        title: const Text(kVirtualName),
        subtitle: const Text('this app\'s own port: Logic and other apps record what you play there',
            style: TextStyle(color: cGrey)),
        trailing: busy ? null : const Icon(Icons.chevron_right, color: cGrey),
        onTap: () => _play(() async => DesktopMidi.openVirtual()),
      ),
      for (final p in ports)
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: Text(p.name),
          subtitle:
              Text([kind(p), 'play it'].where((s) => s.isNotEmpty).join(' · '), style: const TextStyle(color: cGrey)),
          trailing: busy ? null : const Icon(Icons.chevron_right, color: cGrey),
          onTap: () => _play(() async => DesktopMidi.open(p)),
        ),
    ];
  }

  Widget _box(Sighting s) {
    final ways = [
      if (s.onBle) 'bluetooth ${s.rssi ?? '?'} dBm',
      if (s.onWifi) 'wi-fi ${s.host}',
    ].join(' · ');
    // other BLE MIDI devices: through the computer's own MIDI system (the MIDI ports below), not here
    if (s.id == null) return const SizedBox.shrink();
    return ListTile(
      contentPadding: EdgeInsets.zero,
      title: Text(s.name),
      subtitle: Text(ways, style: const TextStyle(color: cGrey)),
      trailing: busy ? null : const Icon(Icons.chevron_right, color: cGrey),
      // BLE works in Play and Setup mode (and in Network mode with BLE kept on); Wi-Fi when that is all there is.
      onTap: () => _open(() => s.onBle ? boxes.openBle(s.bleDevice!, s.name) : boxes.openWifi(s.host!, s.name)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final list = discovery.list.where((s) => s.id != null).toList();
    return Scaffold(
      appBar: AppBar(title: const Text('airmidi'), backgroundColor: cBg, actions: [
        TextButton(
          onPressed: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const TracksPage())),
          child: const Text('tracks'),
        ),
      ]),
      body: ListView(padding: const EdgeInsets.all(14), children: [
        const Text('boxes', style: TextStyle(color: cAc, fontSize: 13)),
        for (final s in list) _box(s),
        if (list.isEmpty)
          const ListTile(
            contentPadding: EdgeInsets.zero,
            title: Text('looking for boxes', style: TextStyle(color: cN)),
            subtitle: Text('over Bluetooth (Play or Setup mode) and on this Wi-Fi (Network mode)',
                style: TextStyle(color: cGrey)),
          ),
        if (busy) const Padding(padding: EdgeInsets.all(12), child: Text('connecting', style: TextStyle(color: cN))),
        if (error != null)
          Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(error!, style: const TextStyle(color: Color(0xFFAB6D6C)))),
        if (discovery.error != null)
          Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(discovery.error!, style: const TextStyle(color: cGrey))),
        const SizedBox(height: 24),
        ..._desktopPorts(),
        ExpansionTile(
          tilePadding: EdgeInsets.zero,
          title: const Text('connect by address', style: TextStyle(color: cN, fontSize: 14)),
          children: [
            Row(children: [
              Expanded(child: TextField(controller: host, decoration: const InputDecoration(hintText: '10.0.0.18'))),
              TextButton(
                onPressed: () => _open(() async {
                  final h = host.text.trim();
                  (await SharedPreferences.getInstance()).setString('host', h);
                  return boxes.openWifi(h, h);
                }),
                child: const Text('connect'),
              ),
            ]),
          ],
        ),
      ]),
    );
  }
}

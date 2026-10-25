// airmidi app (docs/app.md): find boxes over BLE and on Wi-Fi (one entry per box), set them up, play the strips.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'discovery.dart';
import 'link/config.dart';
import 'link/link.dart';
import 'ui/box_page.dart';
import 'ui/strips_page.dart';

void main() => runApp(const App());

class App extends StatelessWidget {
  const App({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'airmidi',
        theme: ThemeData(
          brightness: Brightness.dark,
          scaffoldBackgroundColor: cBg,
          colorScheme: const ColorScheme.dark(primary: cAc, surface: cBg),
        ),
        home: const ConnectPage(),
      );
}

/// Opens a box's page over BLE or Wi-Fi, and follows the box to the other one when a mode change moves it there.
class Boxes {
  final Discovery discovery;
  Boxes(this.discovery);

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

class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key});

  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  final discovery = Discovery();
  late final boxes = Boxes(discovery);
  final host = TextEditingController();
  String? error; // the last connect failure, kept until the next attempt
  bool busy = false;

  @override
  void initState() {
    super.initState();
    discovery.addListener(_changed);
    discovery.start();
    SharedPreferences.getInstance().then((p) => host.text = p.getString('host') ?? '');
  }

  void _changed() => setState(() {});

  @override
  void dispose() {
    discovery.removeListener(_changed);
    discovery.dispose();
    super.dispose();
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

  Widget _box(Sighting s) {
    final ways = [
      if (s.onBle) 'bluetooth ${s.rssi ?? '?'} dBm',
      if (s.onWifi) 'wi-fi ${s.host}',
    ].join(' · ');
    return ListTile(
      contentPadding: EdgeInsets.zero,
      title: Text(s.id == null ? '${s.name} (not an airmidi box)' : s.name),
      subtitle: Text(ways, style: const TextStyle(color: cGrey)),
      trailing: busy ? null : const Icon(Icons.chevron_right, color: cGrey),
      // BLE works in Play and Setup mode (and in Network mode with BLE kept on); Wi-Fi when that is all there is.
      onTap: () => _open(() => s.onBle ? boxes.openBle(s.bleDevice!, s.name) : boxes.openWifi(s.host!, s.name)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final list = discovery.list;
    return Scaffold(
      appBar: AppBar(title: const Text('airmidi'), backgroundColor: cBg),
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
              padding: const EdgeInsets.only(top: 8), child: Text(discovery.error!, style: const TextStyle(color: cGrey))),
        const SizedBox(height: 24),
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
        TextButton(onPressed: () => _open(() async => const StripsPage()), child: const Text('play without a box')),
      ]),
    );
  }
}

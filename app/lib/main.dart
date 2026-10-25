// airmidi app (docs/app.md): find the box over BLE (or type its Wi-Fi address), set it up, play the strips.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:universal_ble/universal_ble.dart';

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

class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key});

  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  final found = <String, BleDevice>{};
  final seen = <String, DateTime>{}; // last advertisement per box: a box that stopped advertising leaves the list
  final host = TextEditingController();
  StreamSubscription<BleDevice>? scanSub;
  Timer? prune;
  String status = '';
  String? error; // the last connect failure, kept until the next attempt
  bool busy = false;

  @override
  void initState() {
    super.initState();
    SharedPreferences.getInstance().then((p) => host.text = p.getString('host') ?? '10.0.0.18');
    _scan();
    prune = Timer.periodic(const Duration(seconds: 2), (_) {
      final old = DateTime.now().subtract(const Duration(seconds: 8));
      final gone = [for (final e in seen.entries) if (e.value.isBefore(old)) e.key];
      if (gone.isEmpty) return;
      setState(() {
        for (final id in gone) {
          found.remove(id);
          seen.remove(id);
        }
      });
    });
  }

  @override
  void dispose() {
    prune?.cancel();
    scanSub?.cancel();
    UniversalBle.stopScan();
    super.dispose();
  }

  Future<void> _scan() async {
    try {
      await UniversalBle.requestPermissions();
      scanSub ??= UniversalBle.scanStream.listen((d) => setState(() {
            found[d.deviceId] = d;
            seen[d.deviceId] = DateTime.now();
          }));
      await UniversalBle.startScan(scanFilter: ScanFilter(withServices: [kBleMidiService]));
      setState(() => status = 'looking for BLE MIDI devices');
    } catch (e) {
      setState(() => status = 'bluetooth: $e');
    }
  }

  Future<void> _open(Future<Widget> Function() page) async {
    if (busy) return;
    setState(() {
      busy = true;
      status = 'connecting';
      error = null;
    });
    try {
      await UniversalBle.stopScan();
      final p = await page();
      if (!mounted) return;
      setState(() => status = '');
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => p));
    } catch (e) {
      setState(() => error = '$e');
    } finally {
      setState(() => busy = false);
      _scan();
    }
  }

  @override
  Widget build(BuildContext context) {
    final devices = found.values.toList()..sort((a, b) => (b.rssi ?? -999).compareTo(a.rssi ?? -999));
    return Scaffold(
      appBar: AppBar(title: const Text('airmidi'), backgroundColor: cBg),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        const Text('bluetooth', style: TextStyle(color: cN)),
        for (final d in devices)
          ListTile(
            title: Text(d.name?.isNotEmpty == true ? d.name! : d.deviceId),
            subtitle: Text('${d.deviceId} · ${d.rssi ?? '?'} dBm'),
            onTap: () => _open(() async {
              final box = await BleBox.connect(d.deviceId, d.name ?? d.deviceId);
              return BoxPage(title: box.name, config: box.config, midi: () async => box.midi, close: box.close);
            }),
          ),
        if (devices.isEmpty) const ListTile(title: Text('none yet: is the box in Play or Setup mode?')),
        const SizedBox(height: 16),
        const Text('wi-fi (network mode)', style: TextStyle(color: cN)),
        Row(children: [
          Expanded(child: TextField(controller: host, decoration: const InputDecoration(hintText: 'box address'))),
          TextButton(
            onPressed: () => _open(() async {
              final h = host.text.trim();
              (await SharedPreferences.getInstance()).setString('host', h);
              final config = HttpConfigClient(h);
              await config.call('GET', '/api/status'); // there, and answering
              return BoxPage(title: h, config: config, midi: () => WsMidiLink.connect(h), close: config.close);
            }),
            child: const Text('connect'),
          ),
        ]),
        const SizedBox(height: 16),
        TextButton(onPressed: () => _open(() async => const StripsPage()), child: const Text('play without a box')),
        if (error != null) Padding(padding: const EdgeInsets.only(top: 12), child: Text(error!, style: const TextStyle(color: Color(0xFFAB6D6C)))),
        if (status.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 12), child: Text(status, style: const TextStyle(color: cN))),
      ]),
    );
  }
}

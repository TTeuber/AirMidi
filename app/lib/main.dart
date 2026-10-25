// airmidi app, spike A0 (docs/app.md): find the box over BLE (or type its Wi-Fi address), then play the strips.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:universal_ble/universal_ble.dart';

import 'link/link.dart';
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
  final host = TextEditingController();
  StreamSubscription<BleDevice>? scanSub;
  String status = '';
  bool busy = false;

  @override
  void initState() {
    super.initState();
    SharedPreferences.getInstance().then((p) => host.text = p.getString('host') ?? '10.0.0.18');
    _scan();
  }

  @override
  void dispose() {
    scanSub?.cancel();
    UniversalBle.stopScan();
    super.dispose();
  }

  Future<void> _scan() async {
    try {
      await UniversalBle.requestPermissions();
      scanSub ??= UniversalBle.scanStream.listen((d) => setState(() => found[d.deviceId] = d));
      await UniversalBle.startScan(scanFilter: ScanFilter(withServices: [kBleMidiService]));
      setState(() => status = 'looking for BLE MIDI devices');
    } catch (e) {
      setState(() => status = 'bluetooth: $e');
    }
  }

  Future<void> _open(Future<MidiLink?> Function() connect) async {
    if (busy) return;
    setState(() {
      busy = true;
      status = 'connecting';
    });
    try {
      await UniversalBle.stopScan();
      final link = await connect();
      if (!mounted) return;
      setState(() => status = '');
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => StripsPage(link: link)));
    } catch (e) {
      setState(() => status = '$e');
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
            onTap: () => _open(() => BleMidiLink.connect(d.deviceId, d.name ?? d.deviceId)),
          ),
        if (devices.isEmpty) const ListTile(title: Text('none yet: is the box in Play or Setup mode?')),
        const SizedBox(height: 16),
        const Text('wi-fi (network mode)', style: TextStyle(color: cN)),
        Row(children: [
          Expanded(child: TextField(controller: host, decoration: const InputDecoration(hintText: 'box address'))),
          TextButton(
            onPressed: () => _open(() async {
              (await SharedPreferences.getInstance()).setString('host', host.text);
              return WsMidiLink.connect(host.text);
            }),
            child: const Text('connect'),
          ),
        ]),
        const SizedBox(height: 16),
        TextButton(onPressed: () => _open(() async => null), child: const Text('play without a box')),
        if (status.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 12), child: Text(status, style: const TextStyle(color: cN))),
      ]),
    );
  }
}

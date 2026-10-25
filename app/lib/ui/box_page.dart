// One box: its state, its mode, Wi-Fi setup, the phones it trusts, and the strips. Works the same over BLE (config
// service) and Wi-Fi (HTTP); the box's API is the same either way.
import 'dart:async';

import 'package:flutter/material.dart';

import '../link/config.dart';
import '../link/link.dart';
import 'strips_page.dart';

class BoxPage extends StatefulWidget {
  final String title;
  final ConfigClient config;
  final Future<MidiLink> Function() midi;
  final Future<void> Function() close;
  const BoxPage({super.key, required this.title, required this.config, required this.midi, required this.close});

  @override
  State<BoxPage> createState() => _BoxPageState();
}

class _BoxPageState extends State<BoxPage> {
  Map<String, dynamic>? status, phones;
  List<String> networks = [];
  String? error;
  Timer? poll;

  ConfigClient get api => widget.config;

  @override
  void initState() {
    super.initState();
    _refresh(all: true);
    poll = Timer.periodic(const Duration(seconds: 3), (_) => _refresh());
  }

  @override
  void dispose() {
    poll?.cancel();
    widget.close();
    super.dispose();
  }

  Future<void> _refresh({bool all = false}) async {
    try {
      final s = await api.call('GET', '/api/status');
      if (all || phones == null) {
        phones = await api.call('GET', '/api/phones');
        networks = List<String>.from((await api.call('GET', '/api/wifi'))['networks'] ?? []);
      }
      if (mounted) setState(() => (status = s, error = null));
    } catch (e) {
      if (mounted) setState(() => error = '$e');
    }
  }

  Future<void> _act(Future<void> Function() f) async {
    try {
      await f();
      await _refresh(all: true);
    } catch (e) {
      if (mounted) setState(() => error = '$e');
    }
  }

  Future<void> _setMode(String mode) async {
    if (mode == 'network') {
      final ok = await showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: const Text('Network mode'),
          content: const Text('The box joins your Wi-Fi and turns Bluetooth off (it restarts). Reconnect to it over '
              'Wi-Fi from the box list.'),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('cancel')),
            TextButton(onPressed: () => Navigator.pop(c, true), child: const Text('switch')),
          ],
        ),
      );
      if (ok != true) return;
    }
    await _act(() => api.call('POST', '/api/mode', {'mode': mode}));
  }

  @override
  Widget build(BuildContext context) {
    final s = status;
    return Scaffold(
      appBar: AppBar(title: Text(widget.title), backgroundColor: cBg),
      body: RefreshIndicator(
        onRefresh: () => _refresh(all: true),
        child: ListView(padding: const EdgeInsets.all(14), children: [
          if (error != null) _banner(error!),
          if (s != null) ..._state(s),
          const SizedBox(height: 18),
          _bigButton('play the strips', () async {
            try {
              final link = await widget.midi();
              if (!context.mounted) return;
              await Navigator.of(context).push(MaterialPageRoute(builder: (_) => StripsPage(link: link)));
            } catch (e) {
              setState(() => error = '$e');
            }
          }),
          const SizedBox(height: 22),
          _heading('wi-fi'),
          for (final n in networks)
            _row(n, trailing: _textButton('forget', () => _act(() => api.call('DELETE', '/api/wifi?ssid=${Uri.encodeQueryComponent(n)}')))),
          if (networks.isEmpty) _row('no networks stored', dim: true),
          const SizedBox(height: 6),
          _bigButton('add a network', () async {
            await Navigator.of(context).push(MaterialPageRoute(builder: (_) => WifiSetupPage(api: api)));
            _refresh(all: true);
          }),
          const SizedBox(height: 22),
          _heading('phones'),
          if (phones != null)
            _row('${phones!['trusted']} of ${phones!['max']} phones trusted · pairing '
                '${phones!['window'] == 'closed' ? 'closed' : phones!['window'] == 'setup' ? 'open (setup mode)' : 'open ${phones!['window_s']} s'}'),
          Wrap(spacing: 8, runSpacing: 8, children: [
            _textButton('add another phone (2 min)', () => _act(() => api.call('POST', '/api/phones/window'))),
            _textButton('forget all phones', () => _act(() => api.call('DELETE', '/api/phones'))),
          ]),
        ]),
      ),
    );
  }

  List<Widget> _state(Map<String, dynamic> s) {
    final wifi = s['wifi'] as Map? ?? {}, bat = s['battery'] as Map? ?? {};
    final ble = (s['ble'] as List? ?? []).cast<Map>().where((b) => b['state'] != 'idle');
    return [
      _heading('mode'),
      Row(children: [
        for (final m in ['play', 'network', 'setup'])
          Padding(
            padding: const EdgeInsets.only(right: 8),
            child: _cell(m, s['mode'] == m, () => _setMode(m), width: 96),
          ),
      ]),
      const SizedBox(height: 14),
      _row('firmware ${s['version']} · ${s['board']}'),
      _row(bat['usb'] == true ? 'on usb power' : 'battery ${bat['pct']} % (${bat['mv']} mV)'),
      _row('wi-fi ${wifi['state']}${(wifi['ip'] ?? '') != '' ? ' · ${wifi['ip']}' : ''}'
          '${(wifi['ssid'] ?? '') != '' ? ' · ${wifi['ssid']}' : ''}'),
      for (final b in ble) _row('ble ${b['state']} · ${(b['interval_ms'] as num).toStringAsFixed(2)} ms · mtu ${b['mtu']}'),
    ];
  }
}

/// Scan from the box (it sees only what it can join: 2.4 GHz), pick one, type the password, let the box try it.
class WifiSetupPage extends StatefulWidget {
  final ConfigClient api;
  const WifiSetupPage({super.key, required this.api});

  @override
  State<WifiSetupPage> createState() => _WifiSetupPageState();
}

class _WifiSetupPageState extends State<WifiSetupPage> {
  List<Map<String, dynamic>> found = [];
  String state = 'scanning';
  String? message;

  @override
  void initState() {
    super.initState();
    _scan();
  }

  Future<Map<String, dynamic>> _wait(String path, bool Function(Map<String, dynamic>) done) async {
    for (var i = 0; i < 60; i++) {
      final r = await widget.api.call('GET', path);
      if (done(r)) return r;
      await Future.delayed(const Duration(milliseconds: 500));
    }
    throw ApiError(504, 'the box took too long');
  }

  Future<void> _scan() async {
    setState(() => (state = 'scanning', message = null));
    try {
      await widget.api.call('POST', '/api/wifi/scan');
      final r = await _wait('/api/wifi/scan', (r) => r['state'] != 'scanning');
      setState(() {
        found = List<Map<String, dynamic>>.from(r['networks'] ?? []);
        state = r['state'] == 'scanned' ? 'pick' : 'error';
        message = r['state'] == 'scanned' ? null : r['reason'];
      });
    } catch (e) {
      setState(() => (state = 'error', message = '$e'));
    }
  }

  Future<void> _try(String ssid, bool secure) async {
    String pass = '';
    if (secure) {
      final ctl = TextEditingController();
      final entered = await showDialog<String>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text(ssid),
          content: TextField(
            controller: ctl,
            obscureText: true,
            autofocus: true,
            decoration: const InputDecoration(hintText: 'password'),
            onSubmitted: (v) => Navigator.pop(c, v),
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c), child: const Text('cancel')),
            TextButton(onPressed: () => Navigator.pop(c, ctl.text), child: const Text('join')),
          ],
        ),
      );
      if (entered == null) return;
      pass = entered;
    }
    setState(() => (state = 'joining', message = 'trying $ssid'));
    try {
      await widget.api.call('POST', '/api/wifi/try', {'ssid': ssid, 'pass': pass});
      final r = await _wait('/api/wifi/try', (r) => r['state'] != 'joining');
      setState(() {
        state = r['state'] == 'joined' ? 'joined' : 'pick';
        message = r['state'] == 'joined'
            ? 'Joined $ssid (the box got ${r['ip']}). Stored: in Network mode the box joins it by itself.'
            : '$ssid: ${r['reason']}';
      });
    } catch (e) {
      setState(() => (state = 'pick', message = '$e'));
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('add a network'), backgroundColor: cBg),
      body: ListView(padding: const EdgeInsets.all(14), children: [
        if (message != null) _banner(message!, good: state == 'joined'),
        if (state == 'scanning' || state == 'joining')
          const Padding(padding: EdgeInsets.all(24), child: Center(child: CircularProgressIndicator(color: cAc))),
        if (state == 'pick' || state == 'error') ...[
          _heading('networks the box can see'),
          for (final n in found)
            InkWell(
              onTap: () => _try(n['ssid'], n['secure'] == true),
              child: _row('${n['ssid']}', trailing: Text('${n['rssi']} dBm${n['secure'] == true ? '' : ' · open'}',
                  style: const TextStyle(color: cGrey, fontSize: 12))),
            ),
          const SizedBox(height: 10),
          _textButton('scan again', _scan),
        ],
        if (state == 'joined') _bigButton('done', () => Navigator.pop(context)),
      ]),
    );
  }
}

// ---- small pieces in the strips' palette ---------------------------------------------------------------------

Widget _heading(String t) => Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Text(t, style: const TextStyle(color: cAc, fontSize: 13, letterSpacing: 0.5)),
    );

Widget _row(String t, {Widget? trailing, bool dim = false}) => Container(
      padding: const EdgeInsets.symmetric(vertical: 10),
      decoration: const BoxDecoration(border: Border(bottom: BorderSide(color: Color(0xFF1A2228)))),
      child: Row(children: [
        Expanded(child: Text(t, style: TextStyle(color: dim ? cGrey : cN, fontSize: 14))),
        if (trailing != null) trailing,
      ]),
    );

Widget _banner(String t, {bool good = false}) => Container(
      margin: const EdgeInsets.only(bottom: 12),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(border: Border.all(color: good ? stripHues[0].base : const Color(0xFFAB6D6C))),
      child: Text(t, style: const TextStyle(color: cN, fontSize: 13)),
    );

Widget _cell(String label, bool selected, VoidCallback onTap, {double width = 44}) => GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: Container(
        width: width,
        height: 44,
        alignment: Alignment.center,
        decoration: BoxDecoration(color: selected ? cAc : Colors.transparent, border: Border.all(color: selected ? cAc : cGrey)),
        child: Text(label, style: TextStyle(color: selected ? cBg : cN, fontSize: 14)),
      ),
    );

Widget _bigButton(String label, VoidCallback onTap) => GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: Container(
        height: 52,
        alignment: Alignment.center,
        decoration: BoxDecoration(border: Border.all(color: cAc, width: 1.5)),
        child: Text(label, style: const TextStyle(color: cAc, fontSize: 16)),
      ),
    );

Widget _textButton(String label, VoidCallback onTap) => GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(border: Border.all(color: cGrey)),
        child: Text(label, style: const TextStyle(color: cN, fontSize: 13)),
      ),
    );

// One box: its state, its mode, Wi-Fi setup, the phones it trusts, and the strips. Works the same over BLE (config
// service) and Wi-Fi (HTTP); the box's API is the same either way.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

import '../link/config.dart';
import '../link/link.dart';
import '../synth/state.dart';
import 'strips_page.dart';

/// Finds the box again on BLE or Wi-Fi after it left the link this page uses, and returns its page (null: not found).
typedef Follow = Future<Widget?> Function(String id, {required bool toWifi, required DateTime since});

class BoxPage extends StatefulWidget {
  final String title;
  final bool overBle;
  final ConfigClient config;
  final Future<MidiLink> Function() midi;
  final Future<void> Function() close;
  final Follow? follow;
  const BoxPage(
      {super.key,
      required this.title,
      required this.overBle,
      required this.config,
      required this.midi,
      required this.close,
      this.follow});

  @override
  State<BoxPage> createState() => _BoxPageState();
}

class _BoxPageState extends State<BoxPage> {
  Map<String, dynamic>? status, phones;
  List<String> networks = [];
  String? error, following;
  bool changing = false; // a mode or settings change is being checked: polls that fail meanwhile are expected
  bool closed = false;
  Timer? poll;

  ConfigClient get api => widget.config;
  late final synth = SynthState(widget.config); // the device's control values, kept while this page is open

  @override
  void initState() {
    super.initState();
    _refresh(all: true);
    poll = Timer.periodic(const Duration(seconds: 3), (_) => _refresh());
  }

  // Once only, and before following: the page that replaces this one connects to the same BLE device id, and
  // closing this link after that (in dispose) would disconnect it.
  Future<void> _close() async {
    if (closed) return;
    closed = true;
    poll?.cancel();
    try {
      await widget.close();
    } catch (_) {}
  }

  @override
  void dispose() {
    synth.dispose();
    _close();
    super.dispose();
  }

  Future<void> _refresh({bool all = false}) async {
    if (following != null) return;
    try {
      final s = await api.call('GET', '/api/status');
      if (all || phones == null) {
        phones = await api.call('GET', '/api/phones');
        networks = List<String>.from((await api.call('GET', '/api/wifi'))['networks'] ?? []);
      }
      if (mounted) setState(() => (status = s, error = null));
    } catch (e) {
      if (mounted && following == null && !changing) setState(() => error = '$e');
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

  /// After a change that may restart the box or move it to the other transport: if this link still answers, stay;
  /// otherwise find the box again (Wi-Fi when it went to Network mode, BLE otherwise) and swap this page for that.
  Future<void> _afterChange({required bool toWifi}) async {
    final id = status?['id'] as String?;
    changing = true;
    await Future.delayed(const Duration(seconds: 2)); // a restart is under way by then (it waits ~0.5 s)
    try {
      await api.call('GET', '/api/status');
      changing = false;
      return _refresh(all: true);
    } catch (_) {
      // gone from this link: follow it
    }
    changing = false;
    final since = DateTime.now(); // the link is gone: only sightings from now on count
    await _close();
    if (id == null || widget.follow == null) {
      setState(() => error = 'The box went away from this connection. Find it again in the box list.');
      return;
    }
    setState(() => following = toWifi ? 'Waiting for the box on Wi-Fi' : 'Waiting for the box over Bluetooth');
    try {
      final page = await widget.follow!(id, toWifi: toWifi, since: since);
      if (!mounted) return;
      if (page != null) {
        Navigator.of(context).pushReplacement(MaterialPageRoute(builder: (_) => page));
        return;
      }
      setState(() => (following = null,
          error = toWifi
              ? 'The box did not show up on this Wi-Fi within 40 s. Has it joined a network (wi-fi below)?'
              : 'The box did not show up over Bluetooth within 40 s.'));
    } catch (e) {
      if (mounted) setState(() => (following = null, error = '$e'));
    }
  }

  Future<void> _setMode(String mode) async {
    if (mode == 'network') {
      final ok = await showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: const Text('Network mode'),
          content: const Text('The box joins your Wi-Fi and, unless "Bluetooth in Network mode" is on in its '
              'settings, turns Bluetooth off and restarts. The app follows it onto the Wi-Fi.'),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('cancel')),
            TextButton(onPressed: () => Navigator.pop(c, true), child: const Text('switch')),
          ],
        ),
      );
      if (ok != true) return;
    }
    try {
      await api.call('POST', '/api/mode', {'mode': mode});
    } on ApiError catch (e) {
      setState(() => error = '$e'); // the box answered: it did not switch
      return;
    } catch (_) {
      // no answer: the switch may still have happened (the box restarts right after answering), so look
    }
    await _afterChange(toWifi: mode == 'network');
  }

  @override
  Widget build(BuildContext context) {
    final s = status;
    return Scaffold(
      appBar: AppBar(title: Text(widget.title), backgroundColor: cBg),
      body: RefreshIndicator(
        onRefresh: () => _refresh(all: true),
        child: ListView(padding: const EdgeInsets.all(14), children: [
          if (following != null) ...[
            _banner(following!, good: true),
            const Padding(padding: EdgeInsets.all(12), child: Center(child: CircularProgressIndicator(color: cAc))),
          ],
          if (error != null) _banner(error!),
          if (s != null) ..._state(s),
          const SizedBox(height: 18),
          _bigButton('play the strips', () async {
            try {
              final link = await widget.midi();
              if (!context.mounted) return;
              await Navigator.of(context)
                  .push(MaterialPageRoute(builder: (_) => StripsPage(link: link, synth: synth)));
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
          const SizedBox(height: 22),
          _heading('settings'),
          _bigButton('settings', () async {
            final restarts = await Navigator.of(context)
                .push<bool>(MaterialPageRoute(builder: (_) => SettingsPage(api: api)));
            if (restarts == true) {
              await _afterChange(toWifi: !widget.overBle);
            } else {
              _refresh(all: true);
            }
          }),
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
      _row('firmware ${s['version']} · ${s['board']} · id ${s['id'] ?? '?'}'),
      _row(bat['usb'] == true ? 'on usb power' : 'battery ${bat['pct']} % (${bat['mv']} mV)'),
      _row('wi-fi ${wifi['state']}${(wifi['ip'] ?? '') != '' ? ' · ${wifi['ip']}' : ''}'
          '${(wifi['ssid'] ?? '') != '' ? ' · ${wifi['ssid']}' : ''}'),
      for (final b in ble) _row('ble ${b['state']} · ${(b['interval_ms'] as num).toStringAsFixed(2)} ms · mtu ${b['mtu']}'),
      _web(s, wifi),
    ];
  }

  // The box's own web page: only while it is on Wi-Fi (Network mode) or has its access point up (Setup mode). The
  // .local name is the box's name; the address always works, the name only where the browser resolves mDNS.
  Widget _web(Map<String, dynamic> s, Map wifi) {
    final ip = wifi['ip'] as String? ?? '';
    final up = (wifi['state'] == 'connected' || wifi['state'] == 'access point') && ip.isNotEmpty;
    if (!up) return _row('web page: in network or setup mode', dim: true);
    Widget link(String url) => GestureDetector(
          onTap: () => launchUrl(Uri.parse(url), mode: LaunchMode.externalApplication),
          child: Padding(
            padding: const EdgeInsets.only(right: 16),
            child: Text(url, style: const TextStyle(color: cAc, fontSize: 14, decoration: TextDecoration.underline)),
          ),
        );
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 10),
      decoration: const BoxDecoration(border: Border(bottom: BorderSide(color: Color(0xFF1A2228)))),
      child: Wrap(runSpacing: 6, children: [
        const Padding(
            padding: EdgeInsets.only(right: 12), child: Text('web page', style: TextStyle(color: cN, fontSize: 14))),
        link('http://$ip'),
        if ((s['name'] as String? ?? '').isNotEmpty) link('http://${s['name']}.local'),
      ]),
    );
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

/// The box's settings (/api/settings). Saving the whole set; some changes restart the box (it says which), and the
/// box page then follows it.
class SettingsPage extends StatefulWidget {
  final ConfigClient api;
  const SettingsPage({super.key, required this.api});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  Map<String, dynamic>? s;
  final name = TextEditingController(), device = TextEditingController(), group = TextEditingController();
  final timeout = TextEditingController();
  String? message;
  bool saving = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final r = await widget.api.call('GET', '/api/settings');
      setState(() {
        s = r;
        name.text = r['name'] ?? '';
        device.text = r['ble_device'] ?? '';
        group.text = r['espnow_group'] ?? '';
        timeout.text = '${r['sysex_timeout_ms'] ?? 500}';
      });
    } catch (e) {
      setState(() => message = '$e');
    }
  }

  Future<void> _save() async {
    setState(() => (saving = true, message = null));
    try {
      final r = await widget.api.call('POST', '/api/settings', {
        ...s!,
        'name': name.text.trim(),
        'ble_device': device.text.trim(),
        'espnow_group': group.text.trim(),
        'sysex_timeout_ms': int.tryParse(timeout.text.trim()) ?? -1,
      });
      if (!mounted) return;
      Navigator.pop(context, r['restart'] == true);
    } catch (e) {
      setState(() => (saving = false, message = '$e'));
    }
  }

  Widget _field(String label, TextEditingController c, String help, {TextInputType? type}) => Padding(
        padding: const EdgeInsets.only(bottom: 14),
        child: TextField(
          controller: c,
          keyboardType: type,
          decoration: InputDecoration(labelText: label, helperText: help, helperMaxLines: 3),
        ),
      );

  Widget _switch(String label, String key, String help) => SwitchListTile(
        contentPadding: EdgeInsets.zero,
        activeColor: cAc,
        title: Text(label, style: const TextStyle(color: cN, fontSize: 14)),
        subtitle: Text(help, style: const TextStyle(color: cGrey, fontSize: 12)),
        value: s![key] == true,
        onChanged: (v) => setState(() => s![key] = v),
      );

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('settings'), backgroundColor: cBg),
      body: s == null
          ? Center(child: message != null ? Text(message!) : const CircularProgressIndicator(color: cAc))
          : ListView(padding: const EdgeInsets.all(14), children: [
              if (message != null) _banner(message!),
              _field('name', name, 'Letters, digits and "-". The Bluetooth name, the Wi-Fi host name and the Setup '
                  'access point. Changing it restarts the box.'),
              _field('bluetooth keyboard', device, 'A BLE MIDI keyboard or controller the box connects to by itself: '
                  'its name or address, "*" for any, empty for none.'),
              _switch('bluetooth in network mode', 'ble_in_network',
                  'Keep Bluetooth on in Network mode. Costs network MIDI ~16 ms of latency (measured).'),
              _switch('esp-now cable', 'espnow', 'A wireless MIDI cable to another airmidi box with the same group.'),
              _field('esp-now group', group, 'Boxes with the same group find each other.'),
              _field('sysex timeout (ms)', timeout, 'How long another source waits behind an unfinished SysEx.',
                  type: TextInputType.number),
              const SizedBox(height: 8),
              saving
                  ? const Center(child: CircularProgressIndicator(color: cAc))
                  : _bigButton('save', _save),
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

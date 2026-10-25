// The box's JSON API (firmware/components/api): over BLE through the config service (framing as in
// lib/include/airmidi/blecfg.h), or over HTTP when the box is on Wi-Fi. The screens use ConfigClient and do not
// care which.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';

const kConfigService = '9a5c0010-3f2b-4d8e-a4b1-6c2e8f1d0a00';
const kConfigRequest = '9a5c0010-3f2b-4d8e-a4b1-6c2e8f1d0a11';
const kConfigResponse = '9a5c0010-3f2b-4d8e-a4b1-6c2e8f1d0a12';
const kLast = 0x01;

/// The box answered with an error status.
class ApiError implements Exception {
  final int status;
  final String message;
  ApiError(this.status, this.message);
  @override
  String toString() => message;
}

/// The box does not trust this phone yet (ATT error 0x08 on the config service).
class NotTrusted implements Exception {
  @override
  String toString() =>
      'This phone is not paired with the box yet. Put the box in Setup mode (hold its button for 8 s), or switch '
      'it off and on and connect within 2 minutes.';
}

abstract class ConfigClient {
  /// The reply's JSON; throws ApiError for a 4xx/5xx answer.
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]);
  Future<void> close();
}

// ---- framing (pure, tested in test/config_test.dart) --------------------------------------------------------

/// [msg] as pieces of at most [pieceMax] bytes, each with the id and the LAST flag on the final one.
List<Uint8List> splitMessage(int id, List<int> msg, int pieceMax) {
  final room = pieceMax - 2;
  final out = <Uint8List>[];
  var off = 0;
  do {
    final end = (off + room < msg.length) ? off + room : msg.length;
    out.add(Uint8List.fromList([id, end == msg.length ? kLast : 0, ...msg.sublist(off, end)]));
    off = end;
  } while (off < msg.length);
  return out;
}

/// Collects reply pieces per id; [add] returns the whole reply once its last piece is in.
class Reassembler {
  final _parts = <int, BytesBuilder>{};

  Uint8List? add(Uint8List piece) {
    if (piece.length < 2) return null;
    final b = _parts.putIfAbsent(piece[0], BytesBuilder.new)..add(piece.sublist(2));
    if (piece[1] & kLast == 0) return null;
    _parts.remove(piece[0]);
    return b.takeBytes();
  }
}

/// "200\n{...}" -> (200, {...}).
(int, Map<String, dynamic>) parseReply(List<int> reply) {
  final text = utf8.decode(reply, allowMalformed: true);
  final nl = text.indexOf('\n');
  final status = int.tryParse(nl < 0 ? text : text.substring(0, nl)) ?? 500;
  final body = nl < 0 ? '' : text.substring(nl + 1);
  final json = body.isEmpty ? <String, dynamic>{} : jsonDecode(body);
  return (status, json is Map<String, dynamic> ? json : {'value': json});
}

Map<String, dynamic> _checked(int status, Map<String, dynamic> json) {
  if (status >= 400) throw ApiError(status, json['error']?.toString() ?? 'error $status');
  return json;
}

// ---- BLE ---------------------------------------------------------------------------------------------------

class BleConfigClient implements ConfigClient {
  final String deviceId;
  final int Function() mtu;
  bool available = false; // the box's firmware has the service (older ones do not)
  final _reassembler = Reassembler();
  final _waiting = <int, Completer<Uint8List>>{};
  StreamSubscription<Uint8List>? _sub;
  Future<void>? _ready;
  var _nextId = 1;
  var _queue = Future<void>.value(); // one request at a time: the box answers them in turn

  BleConfigClient(this.deviceId, this.mtu);

  Future<void> _setup() async {
    if (await UniversalBle.isPaired(deviceId) != true) {
      await UniversalBle.pair(deviceId); // the box's request characteristic needs an encrypted, bonded link
    }
    _sub = UniversalBle.characteristicValueStream(deviceId, kConfigResponse).listen((piece) {
      final whole = _reassembler.add(piece);
      if (whole != null) _waiting.remove(piece[0])?.complete(whole);
    });
    await UniversalBle.subscribeNotifications(deviceId, kConfigService, kConfigResponse);
  }

  @override
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]) {
    final run = _queue.then((_) => _call(method, path, body));
    _queue = run.then((_) {}, onError: (_) {});
    return run;
  }

  Future<Map<String, dynamic>> _call(String method, String path, Object? body) async {
    if (!available) throw ApiError(501, 'this box\'s firmware has no BLE config service: update it');
    _ready ??= _setup();
    try {
      await _ready;
    } catch (e) {
      _ready = null;
      rethrow;
    }
    final id = _nextId;
    _nextId = _nextId % 255 + 1;
    final data = body == null ? <int>[] : (body is List<int> ? body : utf8.encode(jsonEncode(body)));
    final msg = [...utf8.encode('$method $path\n'), ...data];
    final done = Completer<Uint8List>();
    _waiting[id] = done;
    try {
      for (final p in splitMessage(id, msg, mtu() - 3)) {
        await UniversalBle.write(deviceId, kConfigService, kConfigRequest, p);
      }
    } on UniversalBleException catch (e) {
      _waiting.remove(id);
      if (e.code == UniversalBleErrorCode.insufficientAuthorization) throw NotTrusted();
      rethrow;
    }
    final reply = await done.future.timeout(const Duration(seconds: 10), onTimeout: () {
      _waiting.remove(id);
      throw ApiError(504, 'the box did not answer');
    });
    final (status, json) = parseReply(reply);
    return _checked(status, json);
  }

  @override
  Future<void> close() async {
    await _sub?.cancel();
  }
}

// ---- HTTP (Network mode) -----------------------------------------------------------------------------------

class HttpConfigClient implements ConfigClient {
  final String host;
  final _http = HttpClient()..connectionTimeout = const Duration(seconds: 5);
  HttpConfigClient(this.host);

  @override
  Future<Map<String, dynamic>> call(String method, String path, [Object? body]) async {
    final req = await _http.openUrl(method, Uri.parse('http://$host$path'));
    if (body != null) {
      req.headers.contentType = ContentType.json;
      req.add(body is List<int> ? body : utf8.encode(jsonEncode(body)));
    }
    final res = await req.close().timeout(const Duration(seconds: 10));
    final text = await res.transform(utf8.decoder).join();
    final json = text.isEmpty ? <String, dynamic>{} : jsonDecode(text) as Map<String, dynamic>;
    return _checked(res.statusCode, json);
  }

  @override
  Future<void> close() async => _http.close(force: true);
}

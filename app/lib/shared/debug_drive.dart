// Drive the app from outside (tool/drive.py), for checking the desktop app where no screen capture or input
// injection is available. The same commands two ways:
//   - debug builds: service extensions on the Dart VM service (ext.airmidi.<command>), see registerDrive;
//   - the screenshot build (tool/shots_main.dart, a release build, which has no VM service): a web server on
//     localhost (GET /drive/<command>?params), see serveDrive. The app itself (lib/main.dart) never starts it.
// Commands:
//   shot     renders the app to a PNG in the app's temp dir and returns its path (ratio: pixels per logical pixel)
//   tap      a tap at x, y (logical pixels); buttons=2 for a right click
//   tapText  a tap on the middle of the first text on screen that reads "text" (n: the n-th one, from 0)
//   drag     a press at x0, y0 moved in steps to x1, y1 and let go
//   key      a key down or up: physical (USB HID usage) and logical key ids, as the keyboard sends them
//   text     the focused text field's text set to "text" (typing comes through the text input channel, not key events)
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

final driveRoot = GlobalKey();

typedef DriveCommand = Future<Object> Function(Map<String, String> params);

class DriveError implements Exception {
  final String message;
  DriveError(this.message);
}

Future<void> _tap(Offset at, int buttons) async {
  final g = GestureBinding.instance;
  g.handlePointerEvent(PointerDownEvent(position: at, pointer: 900, buttons: buttons));
  await Future.delayed(const Duration(milliseconds: 60));
  g.handlePointerEvent(PointerUpEvent(position: at, pointer: 900));
}

/// The middle of the n-th text on screen that reads [text], in logical pixels.
Offset? _findText(String text, int n) {
  Offset? found;
  var seen = 0;
  void visit(Element e) {
    if (found != null) return;
    final w = e.widget;
    if (w is RichText && w.text.toPlainText() == text) {
      final box = e.renderObject;
      if (box is RenderBox && box.hasSize && box.attached && seen++ == n) {
        found = box.localToGlobal(box.size.center(Offset.zero));
        return;
      }
    }
    e.visitChildren(visit);
  }

  WidgetsBinding.instance.rootElement?.visitChildren(visit);
  return found;
}

final Map<String, DriveCommand> driveCommands = {
  'shot': (params) async {
    final b = driveRoot.currentContext?.findRenderObject() as RenderRepaintBoundary?;
    if (b == null) throw DriveError('no root yet');
    final img = await b.toImage(pixelRatio: double.parse(params['ratio'] ?? '1'));
    final png = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('${Directory.systemTemp.path}/airmidi_shot.png');
    await f.writeAsBytes(png!.buffer.asUint8List());
    return {'path': f.path, 'w': img.width, 'h': img.height};
  },
  'tap': (params) async {
    await _tap(Offset(double.parse(params['x']!), double.parse(params['y']!)),
        int.parse(params['buttons'] ?? '$kPrimaryButton'));
    return {};
  },
  'tapText': (params) async {
    final at = _findText(params['text']!, int.parse(params['n'] ?? '0'));
    if (at == null) throw DriveError('no text "${params['text']}" on screen');
    await _tap(at, int.parse(params['buttons'] ?? '$kPrimaryButton'));
    return {'x': at.dx, 'y': at.dy};
  },
  'drag': (params) async {
    double v(String k) => double.parse(params[k]!);
    final a = Offset(v('x0'), v('y0')), b = Offset(v('x1'), v('y1'));
    const steps = 12;
    final g = GestureBinding.instance;
    g.handlePointerEvent(PointerDownEvent(position: a, pointer: 901));
    var last = a;
    for (var i = 1; i <= steps; i++) {
      await Future.delayed(const Duration(milliseconds: 16));
      final at = Offset.lerp(a, b, i / steps)!;
      g.handlePointerEvent(PointerMoveEvent(position: at, delta: at - last, pointer: 901));
      last = at;
    }
    await Future.delayed(const Duration(milliseconds: 16));
    g.handlePointerEvent(PointerUpEvent(position: b, pointer: 901));
    return {};
  },
  // The way a decoded platform key event goes: HardwareKeyboard (the pressed-keys state), then the key message
  // handler the focus system listens on, so Focus.onKeyEvent sees it as a real key.
  'key': (params) async {
    final phys = PhysicalKeyboardKey(int.parse(params['physical']!));
    final logical = LogicalKeyboardKey(int.parse(params['logical']!));
    final t = Duration(microseconds: DateTime.now().microsecondsSinceEpoch);
    final KeyEvent e = params['down'] == '1'
        ? KeyDownEvent(physicalKey: phys, logicalKey: logical, timeStamp: t, character: params['char'])
        : KeyUpEvent(physicalKey: phys, logicalKey: logical, timeStamp: t);
    HardwareKeyboard.instance.handleKeyEvent(e);
    // deprecated, but in Flutter 3.27 still the path from the platform to the focus system
    // ignore: deprecated_member_use
    ServicesBinding.instance.keyEventManager.keyMessageHandler?.call(KeyMessage([e], null));
    return {};
  },
  'text': (params) async {
    final ctx = FocusManager.instance.primaryFocus?.context;
    final field = ctx == null
        ? null
        : (ctx.widget is EditableText ? (ctx as StatefulElement).state as EditableTextState : null) ??
            ctx.findAncestorStateOfType<EditableTextState>();
    if (field == null) throw DriveError('no text field has the focus');
    final text = params['text'] ?? '';
    field.userUpdateTextEditingValue(
        TextEditingValue(text: text, selection: TextSelection.collapsed(offset: text.length)),
        SelectionChangedCause.keyboard);
    return {};
  },
};

/// Debug builds: the commands as VM service extensions.
void registerDrive() {
  if (!kDebugMode) return;
  for (final c in driveCommands.entries) {
    developer.registerExtension('ext.airmidi.${c.key}', (method, params) async {
      try {
        return developer.ServiceExtensionResponse.result(jsonEncode(await c.value(params)));
      } on DriveError catch (e) {
        return developer.ServiceExtensionResponse.error(-32000, e.message);
      }
    });
  }
}

/// The screenshot build: the commands on localhost only, answering JSON (400 with {"error"} when one fails).
Future<void> serveDrive(int port, {Map<String, DriveCommand> extra = const {}}) async {
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, port);
  server.listen((req) async {
    final name = req.uri.pathSegments.length == 2 && req.uri.pathSegments[0] == 'drive' ? req.uri.pathSegments[1] : '';
    final c = extra[name] ?? driveCommands[name];
    final res = req.response..headers.contentType = ContentType.json;
    try {
      if (c == null) throw DriveError('unknown command "$name"');
      final body = await utf8.decodeStream(req);
      res.write(jsonEncode(await c({...req.uri.queryParameters, if (body.isNotEmpty) 'body': body})));
    } on DriveError catch (e) {
      res
        ..statusCode = 400
        ..write(jsonEncode({'error': e.message}));
    }
    await res.close();
  });
}

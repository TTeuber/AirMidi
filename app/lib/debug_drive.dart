// Debug builds only: drive the app from outside through the Dart VM service (tool/drive.py), for checking the
// desktop app where no screen capture or input injection is available. Two service extensions:
//   ext.airmidi.shot  renders the app to a PNG in the app's temp dir and returns its path
//   ext.airmidi.tap   a tap at x, y (logical pixels)
//   ext.airmidi.key   a key down or up: physical (USB HID usage) and logical key ids, as the keyboard sends them
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

void registerDrive() {
  if (!kDebugMode) return;
  developer.registerExtension('ext.airmidi.shot', (method, params) async {
    final b = driveRoot.currentContext?.findRenderObject() as RenderRepaintBoundary?;
    if (b == null) return developer.ServiceExtensionResponse.error(-32000, 'no root yet');
    final img = await b.toImage(pixelRatio: 1);
    final png = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('${Directory.systemTemp.path}/airmidi_shot.png');
    await f.writeAsBytes(png!.buffer.asUint8List());
    return developer.ServiceExtensionResponse.result(jsonEncode({'path': f.path, 'w': img.width, 'h': img.height}));
  });
  developer.registerExtension('ext.airmidi.tap', (method, params) async {
    final at = Offset(double.parse(params['x']!), double.parse(params['y']!));
    final g = GestureBinding.instance;
    g.handlePointerEvent(PointerDownEvent(position: at, pointer: 900));
    await Future.delayed(const Duration(milliseconds: 60));
    g.handlePointerEvent(PointerUpEvent(position: at, pointer: 900));
    return developer.ServiceExtensionResponse.result('{}');
  });
  // The way a decoded platform key event goes: HardwareKeyboard (the pressed-keys state), then the key message
  // handler the focus system listens on, so Focus.onKeyEvent sees it as a real key.
  developer.registerExtension('ext.airmidi.key', (method, params) async {
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
    return developer.ServiceExtensionResponse.result('{}');
  });
}

// Debug builds only: drive the app from outside through the Dart VM service (tool/drive.py), for checking the
// desktop app where no screen capture or input injection is available. Two service extensions:
//   ext.airmidi.shot  renders the app to a PNG in the app's temp dir and returns its path
//   ext.airmidi.tap   a tap at x, y (logical pixels)
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
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
}

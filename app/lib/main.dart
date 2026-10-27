// airmidi: one Flutter project, two apps over shared code (docs/desktop.md "Direction"): the phone app
// (lib/mobile, docs/app.md) and the desktop app (lib/desktop, docs/desktop.md); lib/shared is what both use.
import 'dart:io';

import 'package:flutter/material.dart';

import 'package:airmidi/desktop/home_page.dart';
import 'package:airmidi/mobile/home_page.dart';
import 'package:airmidi/shared/debug_drive.dart';
import 'package:airmidi/shared/ui/theme.dart';

void main() {
  registerDrive(); // debug builds: screenshots, taps and keys from tool/drive.py
  runApp(const App());
}

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
        home: Platform.isAndroid || Platform.isIOS ? const MobileHome() : const DesktopHome(),
        builder: (context, child) => RepaintBoundary(key: driveRoot, child: child),
      );
}

// The screenshot build: the app as it ships (a release build: no debug banner) with tool/drive.py's commands on
// localhost, as a release build has no VM service. Its settings live in memory and its projects in a folder of its
// own, so taking screenshots never touches the projects, routes or latency settings of the app in daily use (same
// bundle, same sandbox container). tool/shots.py builds it, drives it and writes docs/images/desktop-*.png.
//   flutter build macos --release -t tool/shots_main.dart
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:airmidi/desktop/daw/project_file.dart';
import 'package:airmidi/desktop/daw/store.dart';
import 'package:airmidi/main.dart';
import 'package:airmidi/shared/debug_drive.dart';

const kShotsPort = 47800;

void main() {
  // ignore: invalid_use_of_visible_for_testing_member
  SharedPreferences.setMockInitialValues({});
  final home = Directory('${Directory.systemTemp.path}/airmidi_shots')..createSync(recursive: true);
  ProjectStore.homeForTests = home.path;
  serveDrive(kShotsPort, extra: {
    // the project the arrangement opens with: the request body, saved as <name>.airmidi and made the last one opened
    'project': (params) async {
      final f = File('${ProjectStore.defaultDir}/${params['name'] ?? 'demo'}.$kProjectExt')
        ..parent.createSync(recursive: true)
        ..writeAsStringSync(params['body']!);
      await (await SharedPreferences.getInstance()).setStringList('daw.recent', [f.path]);
      return {'path': f.path};
    },
  });
  runApp(const App());
}

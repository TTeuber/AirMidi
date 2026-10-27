// Where the project lives on disk (docs/daw.md "Projects and files"). One file per project, saved where the user
// picks (~/Music/airmidi to start with); every 3 s the project is written again if it changed, so there is no
// "save" to forget. An untitled project goes to an autosave file in the app's own folder until it gets a name. The
// last project opens again when the app starts. Recent projects are kept by path; the macOS sandbox lets the app
// reopen those under ~/Music (its music folder entitlement), others have to be picked in the open dialog again.
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:airmidi/desktop/daw/history.dart';
import 'package:airmidi/desktop/daw/model.dart';
import 'package:airmidi/desktop/daw/project_file.dart';

class ProjectStore extends ChangeNotifier {
  final Project project;
  final History history;
  ProjectStore(this.project, this.history);

  String? path; // null: untitled (autosaved to [autosavePath])
  final recent = <String>[];
  String? error;
  String _saved = '';
  Timer? _timer;

  static String? homeForTests;
  static String get _home => homeForTests ?? Platform.environment['HOME'] ?? '.';

  /// ~/Music/airmidi (inside the sandbox, the container's Music is the user's own, with the music entitlement).
  static String get defaultDir => '$_home/Music/airmidi';
  static String get autosavePath => '$_home/Library/Application Support/airmidi/autosave.$kProjectExt';

  String get name {
    final p = path;
    if (p == null) return 'untitled';
    final base = p.split(Platform.pathSeparator).last;
    return base.endsWith('.$kProjectExt') ? base.substring(0, base.length - kProjectExt.length - 1) : base;
  }

  /// Opens the last project (or the untitled autosave), then keeps saving.
  Future<void> start() async {
    final prefs = await SharedPreferences.getInstance();
    recent.addAll(prefs.getStringList(_key) ?? const []);
    for (final p in [if (recent.isNotEmpty) recent.first, autosavePath]) {
      if (File(p).existsSync()) {
        try {
          _load(p, untitled: p == autosavePath);
          break;
        } catch (_) {}
      }
    }
    _saved = encodeProject(project);
    _timer = Timer.periodic(const Duration(seconds: 3), (_) => save());
    try {
      Directory(defaultDir).createSync(recursive: true);
    } catch (_) {}
    notifyListeners();
  }

  /// Writes the project if it changed since the last write (or always, with [force]).
  Future<void> save({bool force = false}) async {
    final text = encodeProject(project);
    if (!force && text == _saved) return;
    final to = path ?? autosavePath;
    try {
      final f = File(to);
      f.parent.createSync(recursive: true);
      final tmp = File('$to.tmp');
      await tmp.writeAsString(text, flush: true);
      await tmp.rename(to); // never half a file
      _saved = text;
      error = null;
    } catch (e) {
      error = 'could not save: $e';
    }
    notifyListeners();
  }

  void _load(String p, {bool untitled = false}) {
    final loaded = decodeProject(File(p).readAsStringSync());
    project.restore(loaded);
    history.clear();
    path = untitled ? null : p;
    _saved = encodeProject(project);
  }

  /// Opens [p]; what was open is saved first.
  Future<void> open(String p) async {
    await save();
    try {
      _load(p);
      _remember(p);
      error = null;
    } catch (e) {
      error = 'could not open ${p.split('/').last}: $e';
    }
    notifyListeners();
  }

  /// A new untitled project, as the app starts with one.
  Future<void> newProject(Project fresh) async {
    await save();
    project.restore(fresh);
    history.clear();
    path = null;
    _saved = '';
    await save(force: true);
    notifyListeners();
  }

  /// Saves under a new name and goes on saving there.
  Future<void> saveAs(String p) async {
    if (!p.endsWith('.$kProjectExt')) p = '$p.$kProjectExt';
    path = p;
    await save(force: true);
    _remember(p);
  }

  static const _key = 'daw.recent';

  void _remember(String p) {
    recent
      ..remove(p)
      ..insert(0, p);
    if (recent.length > 8) recent.removeRange(8, recent.length);
    SharedPreferences.getInstance().then((prefs) => prefs.setStringList(_key, recent));
    notifyListeners();
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }
}

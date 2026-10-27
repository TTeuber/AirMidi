// Undo (docs/daw.md "Model"): before an edit the project is copied whole; undo puts the copy back. Projects are
// small (notes and steps), so a copy per edit is cheap, and no edit needs its own undo code. A drag takes one copy
// on its first move, so it is one step (and a click is none).
import 'package:airmidi/desktop/daw/model.dart';

class History {
  final Project project;
  History(this.project);
  final _undo = <Project>[], _redo = <Project>[];
  static const _depth = 200;

  bool get canUndo => _undo.isNotEmpty;
  bool get canRedo => _redo.isNotEmpty;

  /// Call before changing the project.
  void mark() {
    _undo.add(project.snapshot());
    if (_undo.length > _depth) _undo.removeAt(0);
    _redo.clear();
  }

  /// Nothing to undo or redo (a project was opened).
  void clear() {
    _undo.clear();
    _redo.clear();
  }

  bool undo() {
    if (_undo.isEmpty) return false;
    _redo.add(project.snapshot());
    project.restore(_undo.removeLast());
    return true;
  }

  bool redo() {
    if (_redo.isEmpty) return false;
    _undo.add(project.snapshot());
    project.restore(_redo.removeLast());
    return true;
  }
}

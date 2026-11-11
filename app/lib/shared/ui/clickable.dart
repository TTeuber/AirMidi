import 'package:flutter/widgets.dart';

/// The hand cursor over something that acts on a click. A GestureDetector sets no cursor of its own (InkWell and the
/// Material buttons do), so on the desktop a plain one looks dead under the mouse even though clicking works.
class Clickable extends StatelessWidget {
  final Widget child;
  final bool enabled;
  const Clickable({super.key, required this.child, this.enabled = true});

  @override
  Widget build(BuildContext context) =>
      MouseRegion(cursor: enabled ? SystemMouseCursors.click : MouseCursor.defer, child: child);
}

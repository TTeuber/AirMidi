// The phone's ways to play one link (docs/app.md): the strips, piano keys, drum pads. One page holds the link and
// swaps the surface, so switching does not reconnect; the bar's surface button opens the choice.
import 'package:flutter/material.dart';

import 'package:airmidi/mobile/pads_page.dart';
import 'package:airmidi/mobile/touch_keys_page.dart';
import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/strips_page.dart';

enum Surface { strips, keys, pads }

class PlayPage extends StatefulWidget {
  final MidiLink? link;
  final SynthState? synth;
  final bool closeLink; // the page opened the link (a box's): it closes it
  const PlayPage({super.key, this.link, this.synth, this.closeLink = false});

  @override
  State<PlayPage> createState() => _PlayPageState();
}

class _PlayPageState extends State<PlayPage> {
  Surface surface = Surface.strips;

  @override
  void dispose() {
    if (widget.closeLink) widget.link?.close().catchError((_) {});
    super.dispose();
  }

  Future<void> _choose() async {
    final s = await showModalBottomSheet<Surface>(
      context: context,
      backgroundColor: cBg,
      builder: (context) => SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Row(children: [
            for (final s in Surface.values)
              Expanded(
                child: GestureDetector(
                  onTap: () => Navigator.of(context).pop(s),
                  child: Container(
                    height: 64,
                    margin: const EdgeInsets.symmetric(horizontal: 4),
                    alignment: Alignment.center,
                    decoration:
                        BoxDecoration(color: s == surface ? cAc : null, border: Border.all(color: cAc, width: 1.5)),
                    child: Text(s.name, style: TextStyle(color: s == surface ? cBg : cAc, fontSize: 16)),
                  ),
                ),
              ),
          ]),
        ),
      ),
    );
    if (s != null && mounted) setState(() => surface = s);
  }

  @override
  Widget build(BuildContext context) => switch (surface) {
        Surface.strips => StripsPage(link: widget.link, synth: widget.synth, onSurface: _choose),
        Surface.keys => TouchKeysPage(link: widget.link, synth: widget.synth, onSurface: _choose),
        Surface.pads => PadsPage(link: widget.link, synth: widget.synth, onSurface: _choose),
      };
}

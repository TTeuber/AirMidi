// The bottom bar the phone's keys and pads pages share with the strips' look: back (or box), synth, the surface
// choice, and the page's own buttons.
import 'package:flutter/material.dart';

import 'package:airmidi/shared/link/link.dart';
import 'package:airmidi/shared/synth/state.dart';
import 'package:airmidi/shared/ui/synth_page.dart';
import 'package:airmidi/shared/ui/theme.dart';

const kBarH = 64.0;

Widget barButton(String label, VoidCallback? onTap, {bool lit = false}) => Expanded(
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: SizedBox(
          height: kBarH,
          child: Center(
            child: Container(
              height: 40,
              margin: const EdgeInsets.symmetric(horizontal: 4),
              alignment: Alignment.center,
              decoration: BoxDecoration(
                  color: lit ? cAc : null, border: Border.all(color: onTap != null ? cAc : cGrey, width: 1.5)),
              child: Text(label, style: TextStyle(color: lit ? cBg : (onTap != null ? cAc : cGrey), fontSize: 15)),
            ),
          ),
        ),
      ),
    );

/// back/box, synth, the surface's name (opens the choice), then [extra].
Widget surfaceBar(BuildContext context, String surface,
        {required MidiLink? link,
        required SynthState? synth,
        required VoidCallback? onSurface,
        List<Widget> extra = const []}) =>
    SizedBox(
      height: kBarH,
      child: Row(children: [
        barButton(synth == null ? 'back' : 'box', () => Navigator.of(context).maybePop()),
        barButton(
            'synth',
            synth == null || link == null
                ? null
                : () =>
                    Navigator.of(context).push(MaterialPageRoute(builder: (_) => SynthPage(synth: synth, link: link)))),
        barButton(surface, onSurface),
        ...extra,
      ]),
    );

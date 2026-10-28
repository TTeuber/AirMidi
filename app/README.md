# airmidi app

One Flutter project, two apps (`lib/main.dart` picks by platform):

- **Phone** (`lib/mobile`, Android; plan `../docs/app.md`): find the box over BLE (or by its address on Wi-Fi), set
  it up (mode, Wi-Fi from the box's own scan, trusted phones), play the pedal steel strips (from
  `modeling/proto/pedal_steel`) as MPE over BLE MIDI, the box's WebSocket, or Android's MIDI service to a computer.
- **Desktop** (`lib/desktop`, macOS now, Windows and Linux later; plan `../docs/desktop.md`): the same box pages, the
  computer's MIDI ports through `packages/midi_engine` (libremidi over ffi), the play page (keys and the synth's
  controls), the router, and the arrangement (a MIDI-only DAW, `lib/desktop/daw`, design in `../docs/daw.md`).

Shared (`lib/shared`):

- `strips/strips.dart`: strip geometry and touch logic, a port of the prototype's `strips.js` (tests ported too).
- `midi/mpe.dart`: strip events -> MPE, lower zone, strip n on channel 2 + n, bend ±48. Note messages queue in
  order; bends coalesce to the newest per channel, so a slow link sends fewer, newer bends instead of a backlog.
- `midi/packets.dart`: BLE MIDI packets (timestamped, split at the MTU) and UMP words for `/ws`.
- `link/link.dart`: one BLE connection per box (BLE MIDI + config service; universal_ble, as flutter_blue_plus
  is no longer BSD) and the WebSocket link. `link/config.dart`: the box's JSON API over the BLE config service
  (framing as in `lib/blecfg.h`) or HTTP; pairs on first use, explains ATT 0x08 (phone not trusted yet).
- `boxes.dart`, `ui/box_page.dart`: the box screen (each app says how to play it) and the Wi-Fi setup page.
- `synth/`: the MIDI-CI control list (`controls.dart`), sections and widget groups from `paramPath` (`groups.dart`),
  the values kept per box (`state.dart`); `ui/synth_page.dart` with `envelope_editor`, `xy_pad`, `wave_icon`.
- `play/player.dart`: the strips' sender, one batch in flight at a time. `play/keys.dart`: keyboard and pads logic.
- `ui/strips_page.dart`: raw pointers (`Listener`) and a `CustomPainter`; `ui/theme.dart`: the colours.
- `debug_drive.dart` + `tool/drive.py`: screenshots, taps and keys through the Dart VM service (debug builds), or
  over localhost in the screenshot build (`tool/shots_main.dart`, a release build: no debug banner, its own settings
  and project folder). `tool/shots.py` builds it and takes the README's desktop screenshots (`--window WxH` sets the
  window's size); the phone's are `adb exec-out screencap -p` from a release APK.

Phone: `flutter run` with the phone on USB debugging, `flutter build apk --release`. Desktop: `flutter build macos
--debug`. Tests: `flutter test`. The project pins Gradle 8.11 / AGP 8.7 / Kotlin 2.1 so it builds with Java 21 on
Flutter 3.27.

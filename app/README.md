# airmidi app

Android app for the airmidi box (Flutter). Plan and phases: `../docs/app.md`. Now: spike A0, the pedal steel
strips (from `modeling/proto/pedal_steel`) sending MPE to the box over BLE MIDI or its WebSocket.

- `lib/strips/strips.dart`: strip geometry and touch logic, a port of the prototype's `strips.js` (tests ported too).
- `lib/midi/mpe.dart`: strip events -> MPE, lower zone, strip n on channel 2 + n, bend ±48. Note messages queue in
  order; bends coalesce to the newest per channel, so a slow link sends fewer, newer bends instead of a backlog.
- `lib/midi/packets.dart`: BLE MIDI packets (timestamped, split at the MTU) and UMP words for `/ws`.
- `lib/link/link.dart`: BLE (universal_ble; flutter_blue_plus is no longer BSD) and WebSocket links.
- `lib/play/player.dart`: one batch in flight at a time; the bar under the strips shows batches/s, messages/s, how
  long the link took to take a batch (send) and how long an event waited for its batch (wait), mean/max per second.
- `lib/ui/strips_page.dart`: raw pointers (`Listener`) and a `CustomPainter`; portrait, full screen.

Run on the phone (USB debugging on): `flutter run` here. Tests: `flutter test`.
The project pins Gradle 8.11 / AGP 8.7 / Kotlin 2.1 so it builds with Java 21 on Flutter 3.27.

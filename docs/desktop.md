# The desktop app (plan, 2026-10-26)

The same Flutter app as `app/`, built for macOS first and for Windows and Linux later. Why:

- **Finding the box.** The web page needs the box's `airmidi-<id>.local` name, and the box has no display to show
  it. The app finds the box over Bluetooth, as the phone does. The web page stays as the option that needs no
  install. (A small win for it meanwhile: the phone's box page could show the box's web address.)
- **Room to grow.** Tracks and a sequencer would strain the box's flash and its web server. On the computer they
  cost nothing on the box.
- **Other ways to play.** No strips without a touch screen. Instead: the computer keyboard as a piano, mouse and
  trackpad pads, and later more (the phone gets new ways to play too).

## What carries over from the phone app

Box setup and the config API (the same JSON over BLE; universal_ble has macOS, Windows and Linux backends, to be
tried), the synth page with its widgets (`lib/synth`, `ui/envelope_editor`, `ui/xy_pad`), MIDI parsing and MPE
(`lib/midi`), and the link model (`MidiLink`). Layouts sized for a phone get a wider arrangement where needed.

## MIDI on the desktop

One native MIDI engine for all three desktops, reached from Dart through `dart:ffi`. It is built on
[libremidi](https://github.com/celtera/libremidi) (BSD-2 / MIT; CoreMIDI, Windows MIDI Services, ALSA, PipeWire,
JACK; a C API; MIDI 2.0 UMP on several backends). Android keeps its Kotlin bridge (`MainActivity.kt`), and one Dart
interface covers both.

The engine does what Dart cannot do on time:

- **Ports:** lists inputs and outputs and reports changes. A virtual "airmidi" port lets a DAW record from the app
  and send to it. CoreMIDI and ALSA have virtual ports; on Windows that needs Windows MIDI Services (to check which
  Windows versions have it).
- **Input with timestamps:** every message is stamped with the host clock when it arrives, so recordings line up
  however late Dart reads them.
- **Scheduled output:** Dart plans events ~100 ms ahead with their times. A high-priority native thread (or
  CoreMIDI's own timestamps on macOS) sends each one on time. Dart's timers are a few ms off; that is fine for
  planning, not for playing.

## Tracks

One tracks page and one transport (tempo, play/stop, loop, count-in). Each track sends to one port and channel
(or, for MPE, a zone), and is one of two kinds:

- **Clip track:** recorded or drawn MIDI, for synths. It records live playing from any input: the phone's strips
  (over BLE or USB), the computer keyboard, a USB controller, the box. A clip stores the raw timestamped messages,
  so MPE survives as it was played: each note on its own channel with its own bends and pressure. The editor shows
  notes from the note on/off pairs, with each note's bend drawn along it. Export as a Standard MIDI File.
- **Step track:** a grid, for drum machines. Rows are notes (a drum map), columns are steps; per-step velocity,
  probability later.

### Who keeps time (recommendation)

**The app leads by default.** The sequencer runs on the computer, so its own clock is the steadiest. It sends MIDI
clock, Start/Stop and Song Position to the ports you pick (a drum machine, the box).
That covers the setup where the app is the center.

**"Follow" as a second mode, for Logic.** Logic sends MIDI clock but, as far as I found, cannot follow it (its
documented slave sync is MIDI timecode). So in a Logic session, Logic leads and the app follows its clock and Song
Position. An incoming clock jitters (24 ticks per quarter note over USB or BLE), so the app smooths it into a
tempo instead of stepping on each tick.

Later, if wanted: Ableton Link, a tempo shared over the network that Ableton and many apps support, but not Logic.

## Phases

- **D0:** the existing app built for macOS: box over BLE, its setup pages, the synth page. Finds out whether
  universal_ble works on the Mac as on the phone.
- **D1:** the MIDI engine (libremidi + ffi): ports, the virtual port, timestamped input, scheduled output. Test:
  play a scheduled stream into a loopback and measure the timing.
- **D2:** ways to play: the computer keyboard as a piano (octave, velocity keys), pads.
- **D3:** the transport and step tracks, the app leading the clock.
- **D4:** clip tracks: record from any input, MPE included, play back, export to SMF.
- **D5:** follow mode (clock in from Logic); Windows and Linux builds.

## Status

- **D0 (2026-10-26):** `app/macos` builds (macOS 11+, sandboxed, Bluetooth and network entitlements). On the Mac
  the app finds the box over BLE, opens its page over the config service (as a second central, 30 ms interval),
  the strips and the synth page with the device's current values and widgets. The Android-only parts (Android MIDI,
  "play a Mac") are skipped on the desktop. Debug builds can be driven from the terminal (`lib/debug_drive.dart`,
  `tool/drive.py`: screenshot and tap through the Dart VM service), since the terminal may not capture the screen.
- **D1 (2026-10-26):** `app/packages/midi_engine`, an FFI plugin: libremidi v5.4.3 header-only (vendored, one
  upstream fix applied: v5.4.3's CoreMIDI observer never made its client, so no hotplug), a C API (`src/engine.h`),
  Dart bindings. The connect page lists the computer's MIDI outputs (live as ports come and go) and the app's own
  virtual port "airmidi app"; the strips play any of them. Timing (`test_native`, a loopback through the engine's
  own virtual port, 400 notes 5 ms apart plus a SysEx): all in order, arrival minus due median 0.06 ms, max 0.19 ms,
  once the send thread runs with macOS's time-constraint policy (before: median 1.0 ms, max 5.5 ms, timer
  coalescing). Checked in the app: the strips' MPE setup reached a mido listener on "airmidi app" once (42
  messages), a port made by another process appeared and went away in the list. Not yet tried: the box as a CoreMIDI
  port (Bluetooth in Audio MIDI Setup, or Network MIDI) and input from Dart (the engine side is tested natively).
- **D2 (2026-10-26):** the keys page (`ui/keys_page.dart`, logic in `play/keys.dart` with tests), what the desktop
  opens instead of the strips (a button away): the computer keyboard as a piano (A W S E D ... ' from c, Z/X
  octave, C/V velocity, Tab sustain; physical keys, so any layout), a two-octave piano for the mouse (slides across),
  4 x 4 pads on the GM drum map with velocity by height (channel 10), bend (springs back) and mod wheel. Each note
  ends on the channel and number it started with; everything goes off when the window loses the keyboard. Tried
  through a mido port: notes, octave, velocity, sustain, a held key, a chord, piano and pad clicks, bend and mod
  all arrived as expected. The drive hook gained `keys` (through the focus system's key handler, since macOS would
  not let osascript type).

## Open

- Where projects are saved (one file per project, JSON plus SMF clips?).
- Whether the phone gets the tracks page, and in what form.
- Latency compensation per input (a BLE phone is ~10 ms later than USB) once recording works.

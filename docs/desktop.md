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

## Open

- Where projects are saved (one file per project, JSON plus SMF clips?).
- Whether the phone gets the tracks page, and in what form.
- Latency compensation per input (a BLE phone is ~10 ms later than USB) once recording works.

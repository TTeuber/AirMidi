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
tried), the synth page with its widgets (`lib/shared/synth`, `shared/ui/envelope_editor`, `shared/ui/xy_pad`), MIDI parsing and MPE
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
- **D2 (2026-10-26):** the keys page (`desktop/keys_page.dart`, logic in `shared/play/keys.dart` with tests), what the desktop
  opens instead of the strips (a button away): the computer keyboard as a piano (A W S E D ... ' from c, Z/X
  octave, C/V velocity, Tab sustain; physical keys, so any layout), a two-octave piano for the mouse (slides across),
  4 x 4 pads on the GM drum map with velocity by height (channel 10), bend (springs back) and mod wheel. Each note
  ends on the channel and number it started with; everything goes off when the window loses the keyboard. Tried
  through a mido port: notes, octave, velocity, sustain, a held key, a chord, piano and pad clicks, bend and mod
  all arrived as expected. The drive hook gained `keys` (through the focus system's key handler, since macOS would
  not let osascript type).
- **D3 (2026-10-26):** the tracks page (`desktop/tracks_page.dart`, "tracks" on the desktop's first page): one
  transport (play/stop, bpm, bar.beat), step tracks (drums on the GM map, or an octave of notes for a synth), each
  sending to one MIDI output and channel with its own length (tracks of different lengths drift, polymeter), mute,
  and MIDI clock (Song Position, Start, 24 ticks a beat, Stop) to the ports picked. `desktop/tracks/sequencer.dart` (pure,
  tested: windows join without gaps or doubles, offs before ons at the same time) makes the events of a time window;
  `desktop/tracks/player.dart` hands the next 120 ms to the engine every 20 ms; stop drops what is queued (`ame_clear`) and
  ends the notes that may sound. Measured through a mido port at 120 bpm: steps 250.0 ms apart (+-0.5 ms, the
  listener's own resolution), clock 20.83 ms mean, notes on and off balanced after stop; ten bpm steps up during
  play glided from 250 to 230.8 ms (130 bpm) without a gap or a doubled step. Not saved yet.
- **D4 (2026-10-26), first part:** clip tracks (`ClipTrack` in `desktop/tracks/sequencer.dart`): record from any of the
  computer's MIDI inputs or from the app's own keys page ("keys" on the tracks page; `tracks/keys_bus.dart`), at
  the time the engine stamped each message; an armed track passes what is played through to its output; recording
  loops over the clip's length and adds each pass (overdub); playback loops, every message as recorded, so MPE
  stays one channel per note with its own bends. Drawn as a piano roll. Stop ends a note still held while
  recording, and sends all notes off and centred bends on the clip's channels. Tried with a mido source playing 4
  notes with bends on channel 2 into a one-bar clip: through while recording, then every event back exactly one bar
  (2.000 s) later each loop; the same from the keys page. Not yet: saving, Standard MIDI File export, editing notes,
  latency compensation.

## Direction after D4 (agreed 2026-10-26)

The desktop app is for driving hardware synths and drum machines; Logic is one more destination, its channels set
by hand there.

- **Two apps, one Flutter project.** The desktop gets its own pages instead of the phone's (more screen: the keys
  next to the synth's controls, a DAW-style window). One project, two entry points (`lib/main_mobile.dart`,
  `lib/main_desktop.dart`) over shared code (`lib/shared`: MIDI, MIDI-CI, links, the box pages, widgets) and
  `lib/mobile`, `lib/desktop`. Separate packages only if the two pubspecs ever need to differ.
- **Router.** Inputs x outputs, routes run inside the engine (C++, on the thread the message arrives on; Dart only
  edits the table). Channels are how things are kept apart: a route passes a set of channels and can move each to
  another (one keyboard: channel 1 to the box's synth, channel 10 to the drum machine; or Logic sending channel 3 to
  "airmidi app" and the router taking it on to a synth). Splits by note range, transpose, message type filters per
  route. The box's own router (matrix, filters, remap) is the same idea, so one settings format for both. macOS
  lets every app hear every input, so the router adds destinations; it cannot hide an input from Logic.
- **MIDI 2.0 groups.** A UMP endpoint has 16 groups of 16 channels; a group is how one MIDI 2.0 connection carries
  what MIDI 1.0 needs separate ports (cables) for. The router's address is therefore (port, group, channel), with
  group 1 for every MIDI 1.0 port. It matters once a MIDI 2.0 endpoint is in play: the box over Network MIDI 2.0
  could offer its outputs (TRS, BLE) as groups, so one session reaches each of them.
- **Tracks become a MIDI-only DAW**, linear arrangement first (a clip launcher later): timeline with clips, a piano
  roll with velocity, controller and per-note bend lanes, step clips, a track header per device, the device's own
  controls and named automation lanes from its MIDI-CI list, undo, projects, MIDI files.
- **Phone:** piano keys and drum pads next to the strips (the keys/pads logic is shared already).

Done 2026-10-26: the split (`lib/shared`, `lib/mobile`, `lib/desktop`; the box page takes each app's play page).

Done 2026-10-26: phone keys and pads (docs/app.md). The router: routes in the engine (`ame_route_set`: input,
output, channel mask, channel map, transpose, note range, message types; run on the input's own thread before Dart
hears the message), `desktop/router.dart` (by port name, reopened when ports come and go, saved), `router_page.dart`
(inputs x outputs grid with activity lights, an inspector per route). The app's own port is on both sides: "from
Logic" (what Logic sends to "airmidi app") and "to Logic". SysEx off by default (MIDI-CI loops). Measured natively: a
routed hop adds 0.039 ms median, 0.084 ms max; filter, remap and transpose as asked. Tried in the app: a keyboard's
channel 2 to one port as channel 1 and its channel 10 to another, nothing crossing over; again after a restart
without opening the page. Not yet: the box's own router edited from this page; groups (MIDI 2.0 ports).

Done 2026-10-26: the play view. A box's play page (`desktop/keys_page.dart`) is the synth's controls on top and the
deck along the bottom (bend, mod, the piano, 4 x 4 pads), so a sound changes while it plays; the computer keys keep
playing after a click on a control. The controls are `SynthControls` (`shared/ui/synth_page.dart`, the phone's synth
page is the same widget in one column): on the desktop columns at least 340 px wide, filled in order up to the visible
height like a newspaper (`fillColumns`; a section that goes on in the next column has its heading again), shared out
when it does not all fit. graphics/rescan sit in the app bar (`synthActions`; rescan moved into SynthState). Without
a synth (a MIDI port) the deck alone, at most 300 px tall. Tried with the box (Wi-Fi) and daisy_synth: its values
from the report (cutoff 660 Hz, resonance 24, the envelopes), filter / filter env + amp / fx + osc in three columns
in a 1200 x 780 window (it scrolls a little at that size); a click on a fader sent nothing, then four keys = 8
messages and a pad = 2 out of the box's TRS.

Order: the DAW, designed in docs/daw.md (phases A1-A6: arrangement, piano roll, sequencer, files, controllers, hardware).
A1, the arrangement, done 2026-10-26 (it replaces the D3/D4 tracks page; status in docs/daw.md). A2, the piano roll's editing, and A3, the sequencer, done 2026-10-27. Next: A4, files.

## Open

- Where projects are saved (one file per project, JSON plus SMF clips?).
- Whether the phone gets the tracks page, and in what form.
- Latency compensation per input (a BLE phone is ~10 ms later than USB) once recording works.

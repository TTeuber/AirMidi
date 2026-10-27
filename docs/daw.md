# The desktop DAW (design, 2026-10-26)

The tracks page grows into a small DAW that does only MIDI: no audio, no plugins. It drives hardware synths and drum
machines (the box's synth, a drum machine over USB, Logic as one more destination). A linear arrangement comes first;
a clip launcher later, over the same clips (see "Later").

What it starts from (docs/desktop.md D3, D4): one transport, step tracks and clip tracks, events planned 120 ms ahead
and sent on time by the engine (`desktop/tracks/sequencer.dart`, `player.dart`), MIDI clock out, recording from any
input with MPE kept. Nothing is saved yet, so the model below replaces the current one without a migration.

## Two kinds of clip, two editors

The user's ask: MIDI clips and pattern clips. Double-clicking a MIDI clip opens a piano roll; double-clicking a
pattern clip opens a step sequencer.

- **MIDI clip:** notes in free time, recorded or drawn, with controller lanes. For synth parts, played lines,
  anything with feel. Edited in the **piano roll**.
- **Pattern clip:** a grid of rows (notes) by steps, the way a drum machine thinks. For beats, bass lines and
  arpeggios that are built step by step. Edited in the **sequencer**.

Any track can hold both kinds. The track says where the MIDI goes; the clip says what plays. A drum machine's track
can then have patterns for the groove and a recorded MIDI clip for a fill, and a synth's track can have a pattern
bass line under a played chorus.

A pattern clip can be turned into a MIDI clip ("to MIDI clip": the steps become notes, so they can get feel or a
fill). There is no way back: a free-time clip does not fit a grid without losing what made it free.

## The window

```
+-------------------------------------------------------------------------------------------+
| transport:  play stop rec  loop  | 120.0 bpm  4/4 | 3.2.1 | clock to: [drums] [box]       |
+----------------+--------------------------------------------------------------------------+
| track headers  | 1        2        3        4        5        6        7     (bars)       |
|                |   [=====loop range=====]                     |playhead                    |
| drums      M S | [pat A ][pat A ][pat B ][pat A ][fill~~]                                   |
|  TR-8 ch10   o | ^ pattern clips                     ^ a MIDI clip on the same track       |
| bass       M S | [pat 1 ................ ][pat 2 ......]                                    |
|  box synth ch1 |                                                                          |
| keys       M S |          [~~~~ take 1 ~~~~~~~~~~~~~~][~~~ take 2 ~~~]                      |
|  Logic ch3   o |                                                                          |
+----------------+--------------------------------------------------------------------------+
| bottom panel: the open clip's editor (piano roll or sequencer)  |  tabs: editor | device    |
|                                                                                           |
+-------------------------------------------------------------------------------------------+
```

- **Arrangement** at the top: tracks down, time across, a bar ruler, the loop range, the playhead. Zoom with
  pinch or cmd-scroll; positions snap to bars, beats or off.
- **Bottom panel**, as in Logic and Ableton: the clip being edited, in its editor. A drag on the divider sizes it.
  The **device** tab is the play view's synth controls (`SynthControls`) for the selected track's MIDI-CI device,
  so the sound can be changed next to the part.
- **Track header:** name and colour, the device (port, channel or MPE zone, the MIDI-CI device behind it if there
  is one), mute, solo, record arm with its input, and monitor (what is played goes through to the device while
  armed, as now).

### Clips on the arrangement

- Draw an empty clip (choose MIDI or pattern), or record one.
- Move, and copy with alt-drag or cmd-D (duplicate after itself). Split at the playhead, delete, copy and paste.
- The right edge sets the clip's length on the timeline. If that is longer than the content, the content loops (a
  one-bar pattern dragged to eight bars plays eight times); shorter cuts it. The left edge trims the start.
- A note still sounding where a clip ends gets its note off there, so cut clips never leave hanging notes.

## The piano roll (MIDI clips)

- Keys down the left with note names; on a drum device, the drum names from its map. Clicking a key plays it on the
  track's device.
- Notes as bars. Draw, select (click, drag a box, cmd-A), move, resize, delete. Alt-drag copies. Arrows move a
  semitone or a grid step; shift-arrows an octave or a bar.
- Grid and snap: 1/4 to 1/32 with triplets, or off. Quantize the selection (start, optionally ends, with a
  strength), transpose, legato, and set or scale velocity.
- **Velocity lane** under the notes: a stalk per note, dragged.
- **Controller lanes:** one per controller in the clip, added from a list. For a MIDI-CI device that list is its
  own controls by name ("Cutoff", "Env Amt", their ranges and named points from the control list) instead of CC
  numbers. Pitch bend, channel pressure, and program change get their own lanes. A lane is breakpoints: steps, or
  ramps that play as a stream of values.
- **MPE:** each note's own bend, pressure and timbre (CC74) are drawn along the note (bend as a line through the
  bar, as the clip painter does now) and can be edited by opening the note's expression.
- Recording into the open clip: overdub (adds, the default) or replace, over the loop range.

## The sequencer (pattern clips)

- Rows by steps. A click toggles a step. A drag up or down on a step sets its velocity, shown as the fill height.
- **Rows** come from a row set:
  - a drum map (General MIDI by default, or the device's own names);
  - a scale (key, scale and octaves, so a bass line only offers notes in key);
  - chromatic (an octave from a chosen note).

  Rows can be added, removed, muted and renamed.
- **Length** 1 to 64 steps, shown in pages of 16, and a step size (1/16 by default, 1/8, 1/32, triplets). Swing
  per pattern.
- **Per step:** velocity, gate (length), probability, nudge (a little early or late), and repeat (ratchets: 2 to 4
  hits in the step). The lane under the grid edits the chosen one for every step in a row, like the piano roll's
  velocity lane.
- **Per row length (later):** rows of different lengths drift against each other, the polymeter the step tracks
  have now per track.
- **Parameter locks (later):** a step can also set a control of the device (the filter opens on step 7 only).
  MIDI-CI names make these readable. It is what makes a step sequencer play a hardware synth well; it comes after
  the basics work.

### Every clip is its own

Every clip, pattern or MIDI, owns its content (user decision 2026-10-26). Duplicating a clip copies it, and a copy
can go to another track (drag it onto that track, or copy and paste), so a drum pattern can be tried on a second
drum machine. Editing one clip never changes another.

## Model

Pure Dart, no Flutter imports, tested like `sequencer.dart` now.

```
Project
  tempo (bpm; tempo changes later), time signature (one; changes later), loop range (times in beats)
  tracks: [Track]
Track
  name, colour, mute, solo
  device: port name, channel (0-15) or MPE zone (manager channel + member count), MIDI-CI device id (optional)
  input (for recording), arm, monitor
  clips: [Clip]                       placements on the timeline
Clip
  start, length (beats on the timeline), offset (where in the content it starts), loops: bool
  content: MidiContent | Pattern      owned by the clip; a copy copies it
MidiContent
  length (beats, the loop length), notes: [Note], lanes: {controller: [Point]}, other: [raw events]
Note
  start, length, pitch, velocity, release velocity, expression: {bend|pressure|timbre: [Point]} (MPE)
Pattern
  rows: [Row(note, name, mute)], steps, step size, swing, cells[row][step]: Step(velocity, gate, prob, nudge, repeat)
```

- **Times** in beats (doubles) everywhere, as now: no ppq rounding inside, so recorded timing is kept exactly. SMF
  export rounds to its ppq.
- **Values** are stored at MIDI 2.0 resolution (16-bit velocity, 32-bit controllers; the MIDI-CI controls already
  are 32-bit) and scaled down for MIDI 1.0 ports. The model does not have to change when MIDI 2.0 ports arrive.
- **Recording turns raw messages into notes and lanes.** That is a change from the current clips, which keep the
  raw bytes. It is needed for editing: a note has to be one thing that moves with its own bend. Messages that are
  neither notes nor controllers (SysEx, for instance) stay raw in `other`.
- **MPE without fixed channels.** An MPE recording stores notes with their expression, not the member channel each
  was played on. Playback hands out member channels afresh (least recently used, as the strips' player does). Copied
  or moved notes then never collide on a channel. On a track that is not MPE, notes go out on the track's channel
  and per-note bends are dropped (with a warning on the track).
- **Undo:** every edit is a command (do / undo) on the project. One stack per project, cmd-Z / shift-cmd-Z. A drag
  is one command, not one per frame.

## Playback

The arrangement compiles into events window by window, as `eventsBetween` does now. Every 20 ms, for the next
120 ms, each track asks the clips that overlap the window for their events (notes cut at the clip's end, pattern
steps with swing, nudge and probability), plus clock.

- **Edits during play** take effect from the next window: at most 120 ms of already planned events play the old
  way. This is simpler and safer than pulling back events already handed to the engine, which risks lost note offs.
- **Probability** is rolled when a window is planned, with a seed per pass, so a loop does not repeat the same
  "random" choices every time unless that is asked for.
- **Starting in the middle:** the last value of each controller before the start position is sent first
  (controller chase, on by default). This matters for hardware: the filter is where the song says, not where it was
  left. Notes that began before the start are not started (note chase off, as in most DAWs; an option later).
- **Clock** out per port as now; follow Logic later (docs/desktop.md D5).
- **Latency offset per port** (ms, positive or negative): a synth over BLE and a drum machine over USB answer at
  different delays, so each port's events move by its offset. Measured once with a loopback; a calibration tool
  later.

## Recording

- Arm a track (or several), press record. Each armed track gets a new MIDI clip from where recording started to
  where it stopped, from its input, at the engine's timestamps, as now.
- **Loop recording:** each pass adds to the clip (overdub), as now. Takes (each pass its own clip, pick the best)
  come later.
- **Count-in** of a bar, and an optional **MIDI click**: a note on a chosen port and channel (a drum machine's rim
  shot), since there is no audio for a metronome.
- **Input latency:** the input's own offset (BLE input arrives a few ms late) is taken off the timestamps.
- Recording into a pattern (step entry while stopped, or live into the steps it lands nearest) comes after the
  sequencer itself.

## Projects and files

- One project is one JSON file (`.airmidi`, readable, with a format version), saved where the user picks
  (the default is `~/Music/airmidi`). Autosave and a recent-projects list. The sandbox needs the
  user-selected-files entitlement.
- **Standard MIDI File** export: type 1, a track per track, patterns rendered to notes. Import: each SMF track
  becomes a track with one MIDI clip.
- **Device state with the project (later):** a MIDI-CI device reports its current values (the MIDI Message Report
  the synth page already asks for). The project can keep them per track and send them back when it opens, so the
  box's synth sounds the same next time. For other hardware: program change and bank select per track, sent on
  open and on play.

## Phases

- **A1, the arrangement:** the model above, tracks with headers, clips of both kinds placed, moved, resized
  (looping), duplicated, split, deleted, zoom, snap, loop range, playhead, undo. The current step tracks and clip
  tracks become a pattern clip and a MIDI clip. Plays through the existing scheduler.
- **A2, the piano roll:** notes, the velocity lane, snap, quantize, transpose, audition, recording into the clip
  (raw recordings turned into notes, MPE included).
- **A3, the sequencer:** the grid, row sets, length and step size, swing, the per-step lane (velocity, gate,
  probability, nudge, repeat), "to MIDI clip".
- **A4, files:** save, open, autosave, recent projects, SMF export and import.
- **A5, controllers:** controller lanes with MIDI-CI names, MPE expression editing, controller chase, the device
  tab.
- **A6, hardware:** program change per track, device state with the project, MIDI click and count-in, latency
  offsets per port, parameter locks.
- **Later:** the clip launcher (a grid of the same clips by scene; the clips' contents are already separate from
  where they sit on the timeline), tempo and time signature changes, takes, track automation over the whole song,
  follow Logic, MIDI 2.0 ports, row lengths per pattern row, mono 303-style patterns (a note, slide and accent per
  step).

Tests as now: the model and the compiler in Dart unit tests (windows join without gaps or doubles, clip ends cut
notes, loops, swing, chase, undo), timing through a mido port, the pages driven with `tool/drive.py`.

## Status

- **A1 (2026-10-26):** `app/lib/desktop/daw/`. `model.dart` (the model above; MIDI 2.0 values with M2-115
  scaling), `compile.dart` (clips to timed events: passes through looping content, notes cut at the clip's end and
  at each pass, the loop's end; patterns with swing, nudge, repeats and probability rolled from the step's identity;
  MPE notes on member channels handed out afresh, each starting from its own bend), `record.dart` (takes: raw input
  rebuilt into notes and lanes every tick, overdub in the loop range or a clip that grows by bars; MPE kept per note,
  detected at three note channels), `history.dart` (undo by snapshot), `player.dart` (the scheduler of D3 over a
  continuous beat count mapped into the loop; clock follows the continuous count; Song Position and Start or
  Continue), `arrange_page.dart` + `timeline.dart` (transport, headers with port, channel, MPE, input, M/S/R;
  ruler: click = start marker, drag = loop range; clips: move across tracks, resize both edges, alt-drag copy,
  right-click menu with rename, duplicate, split, copy, delete; empty space: new MIDI or pattern clip, paste;
  zoom and scroll by trackpad or cmd-scroll; keys: space, R, L, cmd-Z, delete, cmd-D/C/V/T), the bottom panel
  (`pattern_editor.dart`: toggle steps, length, step size, swing, row sets; `piano_roll.dart`: shows the notes and
  each note's bend). Tracks can send to "airmidi app" (to Logic) and record from it. Tests (`test/daw_test.dart`):
  value scaling, clip passes and offsets, windows joining for 20 random splits, swing, repeats, probability,
  MPE channels and bends, solo, the loop cut, takes in a loop, growing takes, an MPE take, undo. Tried in the app
  through mido ports at 120 bpm: a beat and its duplicate with snares added only to the copy (every note on time to
  0.2 ms, the original unchanged), a one-bar loop repeating without a gap, a two-note take recorded from "airmidi
  app" (passed through while recording, then played back exactly 2000 ms later each pass), undo and redo of the
  take, moving it a bar and onto another track, clock (20.83 ms ticks, Start, Stop), split at the start marker.
  The old step and clip tracks (`desktop/tracks*`) are gone. Not tried: alt-drag (the drive hook cannot hold alt).

## Decided (2026-10-26, the user)

1. Every clip is its own (no shared patterns); clips copy to other tracks.
2. Both clip kinds on any track.
3. The editor in the bottom panel.
4. MPE replayed on fresh member channels.
5. Parameter locks later (A6).

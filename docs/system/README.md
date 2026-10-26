# The airmidi system: how the pieces fit together

The box is one piece of a larger set: surfaces that turn a gesture into MIDI (the app's strips, the box's play page,
the next ideas), instruments that make the sound (daisy_synth, later JUCE plugins and sd_synth, any synth on the
market), and links that carry MIDI between them (the box, a phone, a Mac). This page is the contract a future
project implements to fit in. It is standard MIDI first: every piece works with any MIDI 1.0 gear, works better with
MIDI-CI, and works best with another piece of this system, and each step up is asked for, never assumed.

## Roles

| role | does | today |
|---|---|---|
| surface | makes MIDI from a gesture | app strips (MPE), the box's play page |
| editor | reads an instrument's description and draws its controls | the app's synth page, the box's play page |
| instrument | makes sound, answers MIDI-CI if it can | daisy_synth, `devices/mac_standin`, any synth |
| link | carries messages and adds no meaning | the box (TRS, BLE, Wi-Fi), a Mac over BLE or USB |

One program can hold several roles: the app is a surface and an editor. The box is a link that also asks the device
on its TRS jacks for its description and caches it (midi-ci.md), so a phone gets it without its own scan.

## Tiers

| tier | an instrument does | a surface or editor then gives it |
|---|---|---|
| 0: MIDI 1.0 | channel voice messages; MPE if it can bend notes one by one | strips, generic CC faders, a hand-written profile |
| 1: standard MIDI-CI | Discovery, Property Exchange (DeviceInfo, AllCtrlList with `paramPath`, CtrlMapList), Process Inquiry's MIDI Message Report | its own controls drawn by name, real current values, widgets such as an envelope editor |
| 2: our additions | "X-" Property Exchange resources listed in its ResourceList | only what tier 1 cannot say (none defined yet, see below) |

Rules:

- **Ask, then use.** A tier is used only after the other end said it has it: the Discovery Reply's category bits
  for Property Exchange and Process Inquiry, the ResourceList for each resource. Silence leaves the piece one tier
  down, and it keeps working there.
- **Harmless to lower tiers.** Nothing sent for a higher tier may change a lower-tier device. MIDI-CI is Universal
  SysEx (`F0 7E ... 0D`), which a device without it ignores.
- **Standard before ours.** A tier 2 resource is added only once the standard has been checked and does not cover
  it. That check and its result go in the resource's entry below.
- **Plain MIDI on the wire.** No custom transports: UMP over TRS was considered and put off (midi-ci.md,
  "Protocol policy").

## Messages, not transports

The system is defined as MIDI messages: MIDI 1.0 channel voice and MIDI-CI SysEx (or the same in UMP). Any link that
carries them works, and nothing in a message depends on how it travelled. Today's links:

| link | between | carries | MIDI-CI over it |
|---|---|---|---|
| TRS, 31250 baud | box, instrument | MIDI 1.0 bytes | yes: the box's scan |
| BLE MIDI | phone or Mac, box; phone, Mac | MIDI 1.0 in BLE MIDI packets | carried, but the box does not initiate on BLE |
| WebSocket `/ws` | phone or page, box | UMP words, binary frames | yes: a host on the page reads through the box |
| Network MIDI 2.0, RTP-MIDI | Mac, box | UMP / MIDI 1.0 | yes: macOS runs Discovery through the box |
| USB MIDI (phone as a USB device) | phone, Mac | MIDI 1.0 | carried |

The box's settings API (JSON over HTTP or the BLE config service, config-over-ble.md) is not part of the
system: it configures the link, not an instrument.

### Plugins

A JUCE plugin is an instrument whose MIDI arrives through the DAW, and MIDI-CI mostly cannot make that trip. VST3 has
no SysEx event at all. AU and CLAP can carry SysEx, but whether the host passes it on varies. The plan, not built:

- Notes and CCs reach the plugin the normal way, through a track, so the DAW records and automates them. The
  phone plays the Mac over BLE or USB as it does now.
- The plugin's description travels by a side channel: a WebSocket from the plugin, carrying the same MIDI-CI
  messages as the box's `/ws`, found over mDNS. Over it an editor gets DeviceInfo, the AllCtrlList, the
  CtrlMapLists and the current values.
- An editor's control moves still go out as CCs on the MIDI link, not over the side channel. This keeps one path
  for control, which the DAW records.

Open: the mDNS service name; whether the channel should also accept control moves for hosts that do not record
them.

## What an instrument implements

`lib/midici_responder` (C11, no allocation) does all of tier 1 from a table of JSON strings; daisy_synth
(`devices/daisy_synth/engine/controls.cpp`) shows how to generate the table from one parameter list.

Tier 0:
- Responds to the CCs its own docs list, and sends a CC when its own knob moves, so editors follow.
- MPE if it can: lower zone, manager channel 1, the zone set by the MPE Configuration Message (RPN 6), each member
  channel's bend range from RPN 0 (48 semitones by default). The strips send that setup: once on a computer link,
  every 3 s on a box link (app.md, "Strips over MIDI").

Tier 1:
- A Discovery Reply with the Property Exchange bit set, plus the Process Inquiry bit if it reports values.
- A ResourceList, DeviceInfo and AllCtrlList. Each AllCtrlList entry has `title`, `ctrlType`, `ctrlIndex`,
  `channel`, `default`, `priority`, `paramPath`, and a `typeHint` when it is not continuous.
- A CtrlMapList for every `valueSelect`, and for continuous controls with a scale worth naming. A curve's points,
  such as 1 ms ... 3 s on an attack, let an editor draw true times.
- A MIDI Message Report with its current values.
- A new Software Revision Level in Discovery whenever its controls change. The box caches by identity and would
  otherwise keep showing the old list.

Tier 2: each "X-" resource listed in the ResourceList, with its JSON shape written below before any code uses it.

## Control paths (`paramPath`)

M2-117 defines `paramPath` as a JSON Pointer that groups controls ("/Oscillator/A/Envelope/Attack"). Editors use
it to group controls and to pick a widget. Our conventions, so every instrument's paths match:

- Lower camel case segments; one group per function: `/filter/cutoff`, `/env/attack`, `/osc/wave`.
- Several of one kind get a name or number segment: `/amp/env/attack`, `/filter/env/attack`, `/osc/1/wave`. An
  editor titles a widget with its group's segments after the section, leaving out a bare `env`.
- An editor matches the last segment case-insensitively. These groups get their own widget:

| a group with | widget | state |
|---|---|---|
| `attack`, `decay`, `sustain`, `release` (optional `delay`, `hold`) | envelope editor: drag the corners, times from the CtrlMapLists | in the app |
| `cutoff`, `resonance` | XY pad: cutoff across, resonance up, a low-pass curve behind the dot | in the app |
| `rate`, `depth` (optional `wave`) | LFO | idea |

A picker whose value names are wave shapes (saw, square, triangle, sine, pulse, noise) draws icons. Everything else is
drawn as faders, toggles and pickers as now, under a heading from its section. The app's "graphics" switch turns the
widgets off, to compare. Without a
`paramPath` an editor can still try the title ("Amp Attack"), and falls back to a flat list.

daisy_synth sends `/filter/cutoff`, `/filter/resonance`, `/filter/env/*`, `/amp/env/*`, with time maps on every
attack, decay and release and named frequencies on the cutoff.

## Identity

Our devices use the non-commercial manufacturer id `7D 00 00` (a product would need an MMA id), family 0:

| model | device |
|---|---|
| 2 | `devices/mac_standin` |
| 3 | `devices/daisy_synth` |

A new device takes the next model number and adds its row here.

## Tier 2 resources

None yet. Candidates, each to be checked against the standard first:

- Presets and patch names: the standard ProgramList resource may cover listing them, but not saving one.
- Engine choice (sd_synth).
- Layout hints for an editor: which controls share a page.

## State

Done: tiers 0 and 1 on the box (initiator over TRS, cache, page), the app (synth page in sections by `paramPath`,
the envelope editor, current values), daisy_synth and the Mac stand-in (responder). Not done: any tier 2 resource, the plugin side channel, MIDI-CI Profiles, Set Property Data,
Subscriptions.

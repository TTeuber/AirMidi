# daisy_synth: a MIDI-CI synth for the box to find

An eight-voice poly synth on the Electrosmith Seed3 Desktop Dev Kit (Daisy Seed, libDaisy v9). It answers the box's
MIDI-CI scan with `lib/midici_responder`, the same code the Mac stand-in uses, so the box's play page draws the
synth's own controls. Turning a pot or flipping a toggle on the kit sends that control's CC back, and the page follows.

## Cabling

| box (TRS type A) | kit (DIN) |
|---|---|
| MIDI OUT (J2, middle jack) | MIDI IN, through a TRS-A to DIN adapter |
| MIDI IN (J3, next to the switch) | MIDI OUT, through a DIN to TRS-A adapter |

Audio: the kit's line outs (or headphones). Power: the kit's USB-C.

## Build and flash

```
devices/daisy_synth/build.sh flash    # libDaisy in ~/Developer/libDaisy (LIBDAISY_DIR), OpenOCD + J-Link on SWD
devices/daisy_synth/host/build.sh     # desktop check: MIDI-CI scan by lib/midici_initiator, CC mapping, demo.wav
```

106 KB of the Seed's 128 KB internal flash. CPU at 480 MHz (the audio callback): 14 % idle (the reverb), 33 % with all eight voices; the main loop spends
its spare time on the steady load (see Noise).

## The sound

Two oscillators per voice (saw, square or triangle, detuned against each other) -> 12 dB resonant low-pass
(keyboard tracking, its own ADSR scaled by Env Amt and velocity) -> amp with its ADSR. Glide, vibrato
(mod wheel), pitch bend +-2 semitones, sustain pedal. Master: always-on chorus, ping-pong echo (360 ms), reverb.

| control | CC | on the kit | page |
|---|---|---|---|
| Cutoff | 74 | VR1 | XY pad across, named 20Hz .. 20k |
| Resonance | 71 | VR2 | XY pad up |
| Env Amt | 79 | VR3 | fader (0 .. 7 octaves) |
| Amp Attack | 73 | VR4 | envelope, 1ms .. 3s |
| Amp Decay | 75 | VR5 | envelope, 5ms .. 6s |
| Amp Sustain | 85 | VR6 | envelope |
| Amp Release | 72 | VR7 | envelope, 5ms .. 6s |
| Filter Attack, Decay, Sustain, Release | 86, 87, 89, 90 | | envelope, the amp's ranges |
| Space (reverb) | 91 | VR8 | fader |
| Detune, Vibrato, Glide Time, Echo | 94, 1, 5, 93 | | faders |
| Wave | 70 | SW18: one end saw, middle square, other end triangle | picker (icons in the app) |
| Glide | 65 | SW17 | toggle |
| Bend | pitch bend | | fader, springs back |

"Page" is the box's web page; the app draws the envelopes and the XY pad from the control paths
(docs/system/README.md, "Control paths"), and the web page shows those controls as faders. Version 0.2 (2026-10-26)
added the filter envelope; the version is in Discovery, so the box reads the new list instead of its cache.

The synth listens on every channel and sends on channel 1. CC 64 is the sustain pedal, CC 120/123 silence it.

Buttons: the front row S9-S16 plays C3-C4 (C major), the back row S1-S8 C4-C5; they send their notes too. The LED
above a button glows with the envelope of the voice playing that note (a note off the buttons lights the button with
the same pitch class). A sweep across the LEDs at power-up means the firmware is running.

Pots and toggles act when they move: at power-up the synth starts from the control list's defaults (what the page
shows), and a value set from the page holds until that pot is turned again. Reloading the page resets its tiles to
the defaults; moving a control on either side brings them back in step.

## Layout

- `engine/`: platform-free C++17. `controls` is the one parameter table (CCs, defaults, and the MIDI-CI
  AllCtrlList + CtrlMapLists generated from it), `synth` the voices, `fx` chorus/echo/reverb, `device` the MIDI 1.0
  parser, the responder and the kit's outgoing CCs and notes.
- `src/main.cpp`: board glue (UART on the DIN jacks, buttons, pots, toggles, LEDs).
- `host/test.cpp`: the box's initiator scans the device byte by byte over a simulated cable and writes what it read
  to `host/build/ci/`; checks the CC mapping both ways; renders `host/build/demo.wav`.

## Checked (2026-10-14)

- Desktop: full scan OK (ResourceList, DeviceInfo, AllCtrlList of 15 controls, 4 CtrlMapLists; ~1.5 s of wire at
  31250 baud), every JSON parses, page values for Wave/Glide land on the right option both ways.
- On the kit with the box: a forced scan finds "Daisy Poly Synth" in 1.7 s and the page draws its controls; a chord
  and a MIDI 2.0 CC sent on the box's WebSocket reach the synth (3 voices, then 0 after the note offs).
- Not yet: listening to the line out, pot direction, which end of SW17/SW18 is which, button order and the LEDs.

## Noise on the line out

A 1 kHz whine (the audio block rate: 48 samples at 48 kHz) came from the supply current swinging between the audio
callback's FPU/SDRAM work and the main loop's light polling. Measured on the line out with a Fender Link I/O:

| idle, line out | 1k | 3k | 6k |
|---|---|---|---|
| no fix | -88 dB | -90 | -90 |
| steady load (a muted second synth renders in the main loop), scan on the 1 ms tick | -108 | -89 | -94 |
| steady load, scan every 1000 us from the us timer, LEDs sent only on change (shipped) | -106 .. -109 | -99 | -105 |

What did not help: block size 2 (the 1 kHz stayed), padding the callback to a constant 85 % of the block, turning
off or jittering the button scan or the LED transfers once the steady load was on. `make WHINE_TEST=1` builds the
comparison firmware (6 variants, 5 s each, marked by beeps).

What is left (an 8 kHz line at -92 dB, residual 3 kHz) is a ground loop: the kit and the interface both on the
Mac's USB, joined again by the audio cable. It goes away with the kit's USB unplugged and comes back as soon as the
plug's shield touches. Power the kit from a charger or power bank when recording (and unplug the J-Link: it adds its
own comb, which takes ~15 s to die away after unplugging).

## Debugging over SWD

`main.cpp`'s `dbg` holds cpu avg/max, the eight pot readings, buttons, toggles, MIDI bytes in/out, MIDI-CI replies,
the MUID and the voices sounding; read it while the synth runs:

```
A=$(arm-none-eabi-nm -C build/daisy_synth.elf | grep '::dbg$' | cut -d' ' -f1)
openocd -f flash.cfg -c init -c "mdw 0x$A 18" -c exit
```

`dbgNote` plays a note without a cable: write `0x100 | note` for on and `0x200 | note` for off with `mww`.

## MPE

The MPE Configuration Message (RPN 6 on channel 1: CC 101 0, CC 100 6, CC 6 = member count) turns on a lower zone:
each member channel gets its own voice and pitch bend (±48 semitones, or what RPN 0 sets), the manager channel's
bend (±2) moves every voice, and glide between voices is off. A count of 0 turns MPE off again, back to one bend
for all channels. The airmidi app's strips send this on connect. Bends are smoothed over ~3 ms so the 10-15 ms
steps of a BLE link do not zipper. Checked in `host/test.cpp` ("mpe").

## MIDI IN after a line glitch

A UART error (the box resetting, a cable plugged in while running) used to stop MIDI IN for good while the audio
went on: libDaisy only learns of the error once the HAL's DMA abort completes, which it did not, so it kept
"listening" to a dead UART. The main loop now checks the UART's DMA request bit and restarts listening when it is
off (`KeepListening` in src/main.cpp; `dbg.rxRestarts` counts it). It sets the UART up again before listening:
after some errors (a framing error, seen) the HAL's receive state stays "busy" with the DMA request already cleared,
and a plain stop and start is refused forever. Checked over SWD: clearing the DMA request bit with the line idle
(the stuck-busy case) and with notes arriving (an overrun) each gives one restart, and notes play right after. In a
session of box mode changes and restarts it recovered ten times by itself.

# Testing and CI

Three layers, cheapest first.

## 1. Host unit tests (every commit)

`lib/` is plain C11, so it builds on the Mac and in CI with Unity 2.6 (vendored in `test/unity`, MIT) and runs
under AddressSanitizer + UndefinedBehaviorSanitizer with warnings as errors. Most of the logic that can be wrong
in a MIDI interface (parsing, packing, translation, routing, merging) lives there, so most of the tests do too. See
[core.md](core.md) for what they cover.

## 2. CI (`.github/workflows/ci.yml`)

- **host-tests**: the above on Ubuntu (gcc).
- **format**: clang-format 18.1.8 in check mode over everything except the vendored Unity and the throwaway
  spikes. The version is pinned (`.clang-format` names it, CI and `tools/check.sh` run exactly that one through
  pipx/uv) because clang-format output changes between major versions.
- **firmware**: both boards built in the `espressif/idf` image (C6 and S3), binaries uploaded as artifacts.

`tools/check.sh` runs the same three things locally (`--fix` reformats).

## 3. Hardware in the loop (`tools/hil/`)

The rig: the receiver on the Mac's USB (flash, console, power) and an Arturia AudioFuse Studio, also on the Mac's
USB, cabled both ways to the receiver's TRS jacks (through Type A <-> Type B crossovers). Every wireless test
therefore has both ends on the Mac's clock: send over BLE or Wi-Fi, receive from the AudioFuse, and the other way
round. One-way latency, no round trip to halve.

- `latency.py <port>`: one-way latency, loss, duplicates and order between any wireless CoreMIDI port and the
  AudioFuse, both directions.
- `blemidi.swift`: hands a BLE MIDI device to CoreMIDI from the command line (the steps in
  `CoreMIDI/MIDIBluetoothConnection.h`), which Audio MIDI Setup otherwise does by hand. macOS's own MIDI driver
  then picks a 7.5 ms connection interval; straight from CoreBluetooth (bleak) macOS stays at 30 ms. Quirk found
  in spike S2: activation is ignored while another process holds a CoreMIDI client, and the first attempt after
  that fails too, so connect BLE before opening other ports, and retry.
- `cidevices.swift`: what macOS's `MIDICIDeviceManager` discovered (MIDI-CI over BLE and through the box).

Spikes S0-S3 (`spikes/`) used these first; their results are in the plan's decision log.

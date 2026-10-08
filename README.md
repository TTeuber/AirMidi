# airmidi

A battery-powered wireless MIDI interface: it gives any TRS/DIN MIDI device Bluetooth and Wi-Fi, is set up from a
browser, and updates itself over the air. Firmware for an ESP32-C6 / ESP32-S3 on a custom PCB in a 3D-printed box.

Status: phase 1 (foundation) done; the transports come next. [docs/plan.md](docs/plan.md) has the scope,
architecture, order of work and the decision log with every measured result so far.

## What works so far

- Spikes (throwaway, `spikes/`), all passed on the real board with an AudioFuse Studio as the test rig:
  TRS echo (S0), Network MIDI 2.0 with macOS 26 (S1), BLE MIDI next to Wi-Fi on the C6's one radio (S2), MIDI-CI
  discovery and Property Exchange over TRS and through BLE (S3).
- `lib/`: the portable core (UMP, MIDI 1.0 parser/encoder, MIDI 1.0 <-> 2.0 translation, settings rules) with host
  unit tests under ASan/UBSan.
- `firmware/`: boots on both boards, board layer, settings in NVS, version from `git describe`.

## Layout

```
lib/        portable C11 core, no ESP-IDF headers         docs/design/  one note per module
test/       host unit tests for lib/ (Unity)              tools/hil/    hardware-in-the-loop tools for the Mac
firmware/   ESP-IDF project (C6 and S3)                   spikes/       throwaway experiments, results in the plan
```

## Build

```sh
tools/check.sh                    # what CI runs: host tests, clang-format check, firmware for both boards
firmware/build.sh c6 flash        # build and flash the C6 board (s3 for the S3); add `monitor` for the console
```

ESP-IDF v5.5 (`IDF_PATH`, default `~/Developer/esp-idf`), CMake, and `uv` (for the pinned clang-format and the
Python test tools).

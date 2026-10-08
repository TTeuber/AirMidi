# airmidi

A battery-powered wireless MIDI interface: it gives any TRS/DIN MIDI device Bluetooth and Wi-Fi, is set up from a
browser, and updates itself over the air. Firmware for an ESP32-C6 / ESP32-S3 on a custom PCB in a 3D-printed box.

Status: phases 1-3 of [the plan](docs/plan.md) done and tested on the real board: the TRS <-> BLE MIDI bridge,
Wi-Fi modes, the web UI, and signed over-the-air updates with rollback. Network MIDI 2.0 is next. The plan's
decision log has every measured result; [docs/measurements.md](docs/measurements.md) the numbers with their method.

![The web UI's status page, served by the box](docs/images/webui-status.png)

## What it does so far

- **TRS <-> BLE MIDI**, up to two BLE centrals, merged per output (real-time first, SysEx never broken by another
  source). One way through the box: BLE -> TRS 8.4 ms median, TRS -> BLE 9.5 ms, 0 lost in the hardware tests.
- **Wi-Fi modes**: Play (Wi-Fi off), Network (joins stored networks, `<name>.local`), Setup (own access point).
  Long press switches, 8 s press opens Setup, double press sends all notes off.
- **Web UI** at `http://<name>.local`: live traffic per port, battery, signal, mode, settings, Wi-Fi networks,
  firmware update. Plain JS, no framework, a flat borderless style; 18 KB, built into the firmware.
- **Updates**: upload in the browser or fetch the latest GitHub release; images must be signed, a new image proves
  itself with a self-test or the box rolls back; a crash leaves a downloadable core dump.
- **Battery**: gauge on A0, low warning, notes off and deep sleep when empty.

## How it is tested

- `lib/`: 9 suites of host unit tests under AddressSanitizer + UndefinedBehaviorSanitizer, including randomized
  round trips. The CI workflow runs them, the format check and both firmware builds on every push (it has not
  run on GitHub yet: the repo is not published; `tools/check.sh` runs the same steps locally).
- On the hardware, with an Arturia AudioFuse cabled to the box so both ends of every test sit on the Mac's clock:
  `tools/hil/bridge_test.py` (15 checks), `ota_test.py` (13: refused, accepted, rolled back), `release_test.py`
  (8), `web_test.py` (14), `sleep_test.py`, `latency.py`. Their first runs found 7 real bugs; each is in
  [docs/design](docs/design).

## Layout

```
lib/        portable C11 core, no ESP-IDF headers         docs/design/  one note per module
test/       host unit tests for lib/ (Unity)              tools/hil/    hardware-in-the-loop tools for the Mac
firmware/   ESP-IDF project (C6 and S3)                   spikes/       throwaway experiments, results in the plan
web/        the web UI (HTML/CSS/JS, gzipped into the firmware)
```

## Build

```sh
tools/check.sh                    # what CI runs: host tests, clang-format check, firmware for both boards
firmware/build.sh c6 flash        # build and flash the C6 board (s3 for the S3); add `monitor` for the console
```

ESP-IDF v5.5 (`IDF_PATH`, default `~/Developer/esp-idf`), CMake, and `uv` (for the pinned clang-format and the
Python test tools).

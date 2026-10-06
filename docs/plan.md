# airmidi: plan

Written 2026-10-06, before any code. This is the agreed scope and the order of work; decisions made along the way go
into the decision log at the end, and the per-module design notes go into `docs/design/`.

## Purpose

A battery-powered wireless MIDI interface that gives any TRS/DIN MIDI device Bluetooth and Wi-Fi, is set up from a
browser, and updates itself over the air.

Two goals, in this order when they conflict:

1. **Useful:** the wireless layer for wired devices, including the user's own future instruments (sd_synth, Daisy
   pedals). Those devices only need a TRS MIDI in + out and a MIDI-CI responder; the receiver discovers them and the
   web UI draws their controls.
2. **Portfolio:** a firmware repo that shows the things a junior embedded role asks about: ESP-IDF in C, FreeRTOS,
   a layered architecture with a portable core, host unit tests in CI, a hardware-in-the-loop test, signed OTA with
   rollback, measured power and latency, and the hardware (PCB + enclosure) designed by the same person.

Working agreement: Claude writes the first version of everything; the user studies it afterwards and must be able to
explain and defend every line. So: one working step per commit with a message that says why, and a design note per
module (what it does, alternatives considered, how it is tested).

## Hardware

### Receiver (rev A, from the `modeling` workspace)

72 x 46 mm PCB in an 80 x 52 x 27 mm printed box. XIAO module soldered on headers; Li-ion 1050 mAh -> TP4057 charger
module -> slide switch -> TPS63020 buck-boost -> XIAO 3V3. TRS Type A MIDI out (3.3 V loop, 10R/33R) and MIDI in
(6N138 opto, 1k pull-up), status LED, one tactile button, battery divider (1/2) on A0.

| Signal | Header pin | XIAO ESP32-C6 GPIO | XIAO ESP32-S3 GPIO | Notes |
|---|---|---|---|---|
| MIDI TX | D6 | 16 | 43 | UART0 TX on both: the ROM boot log leaves here at reset |
| MIDI RX | D7 | 17 | 44 | idles high, 2x baud headroom measured |
| Battery sense | A0 | 0 | 1 | ADC1, cell / 2; calibrated reading was 14 mV under the meter |
| Status LED | D1 | 1 | 2 | 3 mm LED + 1k, visible through the lid |
| Button | D2 | 2 | 3 | to GND, internal pull-up |
| RF switch power | - | 3 (drive low) | - | C6 module internal |
| Antenna select | - | 14 (low = ceramic) | - | C6 module internal; U.FL path ~20 dB weaker with no antenna |

- Board #1 carries the C6 (soldered, stays). The S3s arrive 2026-10-06; one goes on a second rev A board.
- C6: single-core RISC-V 160 MHz, 512 KB SRAM, 4 MB flash, Wi-Fi 6 + BLE 5 on **one shared radio**, USB serial/JTAG
  only (no USB device: no USB MIDI).
- S3: dual-core Xtensa 240 MHz, 8 MB flash + 8 MB PSRAM, Wi-Fi 4 + BLE 5 (also one radio), **USB OTG** (USB MIDI).
- The console goes to the USB port on both (UART0 is the MIDI port).
- Bring-up already done in `modeling/firmware/midi_receiver/` (radio_test, midi_loop, batt_ble): radios on the
  ceramic antenna, 30208/30208 bytes in the TRS loopback, LED, button, battery sense. Those were throwaway tests;
  airmidi starts from scratch.
- Measured: 40-50 mA with BLE connected and no sleep, ~20 h on the cell.

### Test rig

```
                 USB (flash, console, power)
   Mac  ─────────────────────────────────────── receiver (C6)
    │  BLE MIDI / Wi-Fi                         J2 OUT ──TRS──▶ Pod MIDI IN  (stock, H11L1, D14)
    │                                           J3 IN  ◀──TRS── Pod MIDI OUT (added, D29)
    └── J-Link clone ── SWD ── Daisy Pod (Seed with JTAG header) ── headphones (user listens)
```

- **Daisy Pod MIDI out (user adds):** USART1 TX on **D29 = PB14 (AF4)**, because the usual TX pin D13 (PB6) is the
  Pod's encoder click. USART1 keeps RX on D14, so one libDaisy `MidiUartHandler` does both directions. 3.3 V MIDI out
  per CA-033: TRS ring (pin 4) -> 33R -> 3V3, TRS tip (pin 5) -> 10R -> TX, sleeve to GND. Check D29's Seed pin
  number against the pinout before soldering.
- Flashing the Pod: `openocd -f interface/jlink.cfg -c "transport select swd" -f target/stm32h7x.cfg -c "program
  build/pod_synth.elf verify reset exit"` (OpenOCD 0.12 from Homebrew). The same connection reads variables for
  the automated tests (message counters, last message, CI state), as `peek.sh` did on the H7S7.
- Toolchains: ESP-IDF v5.5.x at `~/Developer/esp-idf`, libDaisy at `$LIBDAISY_PATH` (`~/Developer/libDaisy`),
  arm-none-eabi-gcc 14.3. Mac: macOS 26.5.2.
- Without the Pod, the receiver still tests itself with a TRS cable from J2 to J3 (loopback).

## Features

### Transports

| Transport | Boards | Protocol | Notes |
|---|---|---|---|
| TRS MIDI in/out | both | MIDI 1.0 bytes, 31250 baud | running status, real-time messages inside SysEx, merge |
| BLE MIDI | both | BLE MIDI 1.0 (Apple/MMA spec), peripheral | service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700; 7.5-15 ms connection interval |
| BLE MIDI central | both | same, as central | connects by itself to a BLE MIDI keyboard/controller (phase 6) |
| Network MIDI 2.0 | both | UMP over UDP (M2-124), mDNS discovery | first choice for Wi-Fi if the Mac supports it (spike S1) |
| RTP-MIDI (AppleMIDI) | both | MIDI 1.0 over RTP, `_apple-midi._udp` | works everywhere today (macOS/iOS native, Windows rtpMIDI) |
| ESP-NOW | both | our framing over ESP-NOW | two airmidi boxes as a wireless MIDI cable, no router |
| USB MIDI | S3 only | USB MIDI (2.0 class if TinyUSB supports it) | USB console then moves to a CDC interface |
| Web UI | both | WebSocket from the browser | control surface + config; no app to install |

BLE MIDI 2.0 is still an unpublished draft: BLE stays MIDI 1.0 until it is final.

### MIDI 2.0 / MIDI-CI

- **UMP is the internal format.** Every input is converted to UMP at the edge (MIDI 1.0 bytes -> MT 0x2 channel
  voice, MT 0x3 SysEx7, MT 0x1 system), the router only ever sees UMP, and each output converts back. MIDI 2.0
  channel voice (MT 0x4) from network sources is translated down for MIDI 1.0 outputs using the M2-104 rules.
- **The receiver as a UMP endpoint** on Network MIDI 2.0: Endpoint Discovery, Function Blocks per port (TRS, BLE,
  ESP-NOW, ...).
- **MIDI-CI over TRS** (SysEx, so it works at 31250 baud): the receiver is a CI initiator towards the device on its
  TRS jacks. At boot, on a button press, or from the web UI it sends Discovery, then Property Exchange gets for
  `ResourceList`, `DeviceInfo` and `ChCtrlList` (M2-117 Controller Resources). The JSON is cached in flash, keyed by
  manufacturer/family/model/version, because at ~3 KB/s a control list takes 1-2 s.
- **Pass-through:** CI messages addressed to the receiver's MUID (or broadcast Discovery from the receiver itself)
  are handled locally; everything else is forwarded, so a CI-capable host (macOS `MIDICIDeviceManager`) can talk to
  the device through the box.
- **Hand-written profiles:** for devices without MIDI-CI (almost every synth on the market), a JSON profile in the
  same `ChCtrlList` format can be uploaded from the web UI. The control page does not care where the JSON came from.
- **`midici_responder`**: a small portable C library for the device side (Discovery reply, PE capabilities,
  `ResourceList`/`DeviceInfo`/`ChCtrlList` gets, `ChCtrlList` driven from a static table). The Pod demo synth uses
  it first, then the user's own devices.
- Specs (downloaded to `modeling/reference/`, not redistributable, referenced by number): M2-100 overview, M2-101
  MIDI-CI 1.2.1, M2-102 Profiles, M2-103 Property Exchange common rules, M2-104 UMP + MIDI 2.0 protocol 1.1.2.
  Still to get: M2-117 PE Controller Resources, M2-124 Network MIDI 2.0 (UDP).

### Routing and merge

- Sources: TRS in, BLE (peripheral + central), network sessions, ESP-NOW, USB, web UI. Destinations: the same.
- A route matrix (source x destination) with per-route filters: channel mask, message type mask, channel remap.
  Default: everything to everything, never back to the port it came from.
- Merge onto a MIDI 1.0 byte output: one message at a time. Real-time messages (clock, start/stop) go out at once, even
  inside another source's SysEx. A SysEx in progress holds other sources' non-real-time messages in a queue until it
  ends or times out (configurable, default 500 ms). Running status is re-applied per output.
- Clock: passed through; jitter from each transport measured, no clock generation in v1.

### Device behaviour

- **Modes:** *Play* (BLE + TRS, Wi-Fi off: lowest power and latency), *Network* (Wi-Fi station + network MIDI + web
  UI, BLE still on with coexistence), *Setup* (own access point + web UI, for the first Wi-Fi join or no network).
  The mode is kept in NVS.
- **Button:** short press = MIDI-CI rescan; long press (2 s) = next mode; very long (8 s) = Setup mode.
  Double press = all notes off (panic) on every output.
- **LED patterns:** advertising, connected, MIDI activity flicker, Wi-Fi joining, Setup mode, low battery, OTA in progress.
- **Battery:** gauge from A0 (ADC calibration, averaged, Li-ion discharge curve), shown in the web UI and as the BLE
  Battery Service; warning at 3.5 V, all notes off + deep sleep at 3.3 V.
- **Power:** BLE with light sleep in Play mode (target 10-20 mA), Wi-Fi power save in Network mode; every mode
  measured with the USB power meter and published.

### Web UI

Served by the device (`http://airmidi.local`, or 192.168.4.1 in Setup mode), plain HTML/JS with no framework, gzipped
and embedded in the firmware image so the UI and the firmware always match. Budget ~100 KB.

- **Status:** battery, mode, connections per transport, message counters, firmware version, uptime, last reset reason.
- **Control surface:** faders/knobs/pads drawn from the device's `ChCtrlList` (from MIDI-CI or a hand-written
  profile); a generic CC/note page when there is none. Sends UMP over a WebSocket; incoming CCs move the controls.
- **Config:** device name, route matrix and filters, Wi-Fi networks, mode, SysEx timeout.
- **Update:** upload a firmware image, or check GitHub Releases; progress and the result of the post-update self-test.
- **Monitor:** a live MIDI log (rate-limited).

### OTA

- Partition table for 4 MB (the S3 uses the same layout and has room to spare):

  | Name | Type | Offset | Size |
  |---|---|---|---|
  | nvs | data | 0x9000 | 24 KB |
  | otadata | data | 0xF000 | 8 KB |
  | phy_init | data | 0x11000 | 4 KB |
  | ota_0 | app | 0x20000 | 1792 KB |
  | ota_1 | app | 0x1E0000 | 1792 KB |
  | coredump | data | 0x3A0000 | 64 KB |
  | storage | data (LittleFS) | 0x3B0000 | 320 KB: CI profile cache, uploaded profiles |

- Sources: an HTTP upload from the web UI, and `esp_https_ota` from the latest GitHub Release (TLS with the bundled
  certificates, version compare, board-specific image).
- **Rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image boots as *pending verify*, runs a self-test
  (radios start, NVS readable, web server answers, the TRS loopback if a cable is in J2 -> J3) and only then marks
  itself valid; a crash or failed test reboots into the previous image.
- **Signed images without burning eFuses:** `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` +
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`: the running app checks the new image's signature before switching.
  Release key as a GitHub Actions secret; a separate dev key, gitignored. Secure Boot and flash encryption are
  discussed in the design note but not enabled (irreversible eFuses on the only C6 board).
- Phase 6 stretch: OTA over BLE.

### Reliability

Task watchdog on every task, core dump to flash (shown and downloadable in the web UI after a crash), reset reason
logged, version string from `git describe` embedded, NVS schema version with migration, bounded queues everywhere
with drop counters instead of blocking the radio tasks.

## Architecture

```
 transports (edge)                          core (portable C, host-tested)          transports (edge)
 ┌──────────────┐  bytes  ┌────────────┐   UMP   ┌──────────┐   UMP   ┌────────────┐  bytes  ┌─────────┐
 │ UART RX ISR  │────────▶│ m1 -> UMP  │────────▶│          │────────▶│ UMP -> m1  │────────▶│ UART TX │
 │ BLE MIDI rx  │────────▶│ (per src)  │         │  router  │         │ merge, RS  │────────▶│ BLE tx  │
 │ RTP-MIDI rx  │────────▶│            │         │  filters │         └────────────┘         └─────────┘
 │ NM2 / web    │───────────── UMP ─────────────▶│          │─── UMP ──▶ NM2 / web / USB
 └──────────────┘                                 └────┬─────┘
                                                       │ CI SysEx for our MUID
                                                  ┌────▼─────┐   JSON   ┌────────┐
                                                  │ midi_ci  │─────────▶│ web UI │
                                                  └──────────┘          └────────┘
 board (C6 / S3 pins, antenna, USB)   power (ADC, sleep, modes)   config (NVS)   ota   led/button
```

- **C11** for the firmware and the portable libraries; C++ only on the Daisy side (libDaisy).
- **Portable core** (`lib/`): no ESP-IDF or FreeRTOS headers, compiled for the host in the unit tests and for the
  ESP32 and the Daisy as-is.
- **Tasks:** one router task fed by a UMP queue; transports run in their stack's context (NimBLE host task, lwIP)
  and only push to or pop from queues. UART RX via the driver's event queue. Priorities and stack sizes documented
  in the design note, stack high-water marks shown in the web UI.
- **Board layer:** `CONFIG_AIRMIDI_BOARD_XIAO_C6` / `_XIAO_S3` in Kconfig selects the pin table and the board init
  (C6: RF switch + antenna GPIOs). One `sdkconfig.defaults.<target>` per board.

## Repository layout

```
airmidi/
  README.md                  what it is, photos, demo video, measurements, how to build
  docs/plan.md               this file
  docs/design/*.md           one note per module
  docs/measurements.md       power, latency, jitter, throughput, with method
  lib/                       portable C: ump, midi1 (parser/encoder), translate, router, midici, midici_responder
  test/                      host unit tests (CMake + Unity) for lib/
  firmware/                  ESP-IDF project: main/, components/ (board, uart_midi, ble_midi, net_midi, rtpmidi,
                             espnow_midi, usb_midi, power, config, ota, webui, led_button), partitions.csv,
                             sdkconfig.defaults.esp32c6 / .esp32s3
  web/                       web UI source, built into firmware/components/webui
  devices/pod_synth/         Daisy Pod demo synth (libDaisy, midici_responder), Makefile, flash.sh (OpenOCD J-Link)
  tools/                     hil/ (Python: bleak, rtmidi, serial, OpenOCD reads), sign/, release helpers
  hardware/                  exported from the modeling workspace: schematic PDF, KiCad project + bundled libs,
                             Gerbers, BOM, STEP/STL, renders (do not edit here)
  .github/workflows/         build C6 + S3 (espressif/idf image), host tests, format/lint, release on tag
```

`hardware/` is a copy, not a link (git stores symlinks as links, which would point outside the repo on GitHub).
An export script in the modeling workspace refreshes it; the KiCad project gets the symbols, footprints and 3D models
it uses bundled, so it opens on another machine.

## Testing

- **Host unit tests** (every commit, CI): MIDI 1.0 parser (running status, real-time inside SysEx, SysEx cut short,
  data bytes without status), UMP conversion both ways, 7 <-> 16/32-bit scaling (M2-104 rules), router filters and remap,
  merge (SysEx hold, real-time priority, timeout), BLE MIDI packet encode/decode (timestamps, wraparound, MTU
  splitting), MIDI-CI message build/parse and PE chunking, responder answers.
- **On-target tests:** ESP-IDF unity test app for the drivers (UART loopback with J2 -> J3, NVS, partition/OTA state).
- **Hardware-in-the-loop** (`tools/hil/`, run from the Mac with both boards attached):
  - BLE note in -> TRS out -> Pod counters over SWD: every message arrives, in order.
  - Pod knob CC -> receiver TRS in -> BLE out to the Mac.
  - Round-trip latency and jitter per transport (Mac -> receiver -> Pod -> receiver -> Mac, halved), with Wi-Fi off
    and on, reported in `docs/measurements.md`.
  - MIDI-CI: discovery + `ChCtrlList` from the Pod shows up in the web UI's JSON endpoint.
  - OTA: upload, self-test, mark valid; and a deliberately broken image rolls back.
- **The user's ears:** the Pod's synth on headphones for the sound checks.

## Order of work

### Spikes first (throwaway code in `spikes/`, deleted or folded in later; results go into the decision log)

- **S0 rig:** Pod MIDI out soldered; OpenOCD + J-Link flashes the Pod; a minimal Pod program plays notes from TRS
  in and echoes them to TRS out; SWD reads of its counters work.
- **S1 Network MIDI on the Mac:** does macOS 26.5 find a Network MIDI 2.0 endpoint the ESP32 advertises (Audio MIDI
  Setup, a CoreMIDI client)? CoreMIDI has no public Network MIDI 2.0 API, only `MIDINetworkSession` (RTP-MIDI). If
  it does not: RTP-MIDI first, Network MIDI 2.0 later for Windows and future macOS.
- **S2 radio sharing on the C6:** BLE MIDI latency/jitter with Wi-Fi off, Wi-Fi connected but idle, and Wi-Fi busy.
  Decides how Network mode treats BLE.
- **S3 MIDI-CI over TRS:** receiver sends Discovery + one PE get; the Pod answers from a hard-coded reply. Also: does
  macOS `MIDICIDeviceManager` see the Pod through the receiver's BLE MIDI?

### Phases (each ends working, tested and committed)

1. **Foundation:** repo skeleton, CI (build both boards, host tests, format check), board layer, `lib/` UMP +
   MIDI 1.0 parser/encoder + translation with tests, logging, version string, NVS config.
2. **Bridge:** TRS in/out, BLE MIDI peripheral, router + merge, LED + button + modes (Play only), battery gauge +
   low-battery shutdown, light sleep; HIL test with the Pod; power and latency measured.
3. **Wi-Fi + OTA:** Setup AP + station join, mDNS, web UI shell (status, config, update), partition table, signed OTA
   with rollback and self-test, GitHub Release builds.
4. **Network MIDI:** Network MIDI 2.0 or RTP-MIDI per S1 (the other one later), UMP endpoint + function blocks,
   WebSocket control surface (generic page).
5. **MIDI-CI:** initiator over TRS, PE gets, profile cache, pass-through, hand-written profile upload, generated
   control page; `midici_responder` library and the Pod demo synth using it. Demo video: the web page discovers the
   Pod, draws its controls, a fader moves the filter.
6. **Extras, in any order:** USB MIDI on the S3, ESP-NOW cable between two boxes, BLE central, the second network
   transport, OTA over BLE.

### Pod demo synth

One voice (saw/square oscillator, low-pass filter with resonance, ADSR), about six CC parameters (cutoff, resonance,
attack, decay, release, waveform) described in its `ChCtrlList`. The Pod's knobs control cutoff and resonance and also
send their CCs out of the new MIDI out; the encoder picks the waveform; the LEDs show MIDI in and CI activity.
Oscillator and filter from DaisySP (to install next to libDaisy) or written by hand, whichever is smaller.

## Open questions

- License (decide before the repo goes public): MIT or Apache-2.0 for code, CERN-OHL-P for hardware.
- Which S3 board gets mounted (second rev A board?), and when.
- Network MIDI 2.0 vs RTP-MIDI first: spike S1.
- BLE + Wi-Fi together on the C6: spike S2.
- USB MIDI 2.0 class support in TinyUSB on the S3: check before phase 6.
- Get the M2-117 (PE Controller Resources) and M2-124 (Network MIDI 2.0) specs from midi.org.
- GitHub: repo created under the user's account when they say so; nothing is pushed until then.

## Decision log

| Date | Decision | Why |
|---|---|---|
| 2026-10-06 | Separate repo `airmidi`, in the modeling workspace's gitignored `portfolio/` | portfolio piece; hardware copied in by an export script |
| 2026-10-06 | Claude writes v1, the user studies it afterwards | prove everything works first (CoreMIDI, radios, OTA); the user must be able to defend every line |
| 2026-10-06 | Support both the C6 (board #1) and the S3 | C6 is soldered on board #1; the S3 is needed for USB MIDI; the C6 may be dropped later |
| 2026-10-06 | Wi-Fi is a full MIDI transport, not only config | user's choice |
| 2026-10-06 | UMP as the internal format | one router for every transport, ready for MIDI 2.0 sources, same approach as CoreMIDI / Windows MIDI Services |
| 2026-10-06 | BLE MIDI stays 1.0 | BLE MIDI 2.0 is an unpublished draft |
| 2026-10-06 | MIDI-CI over TRS for device discovery and control layouts, with hand-written profiles as fallback | works at 31250 baud; almost no commercial gear implements Property Exchange yet |
| 2026-10-06 | C11 for firmware and libs | closest to embedded job listings; ports to Daisy/STM32 |
| 2026-10-06 | Daisy Pod as the device under test, MIDI out added on D29 (PB14) | the Pod has MIDI in only; D13 is its encoder click |
| 2026-10-06 | Spikes S0-S3 before phase 1 | test what is uncertain before building on it |

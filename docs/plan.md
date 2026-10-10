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
            USB (flash, console, power)                          USB (class-compliant MIDI)
   Mac ─────────────────────────────── receiver (C6)        ┌──────────────────────── Arturia AudioFuse Studio
    │                                  J2 OUT ──TRS A↔B──▶ MIDI IN  ┘
    │  BLE MIDI / Wi-Fi                J3 IN  ◀──TRS A↔B── MIDI OUT
    └─ (the same Mac sees the receiver over the air and the AudioFuse over USB)
```

- **Both ends on one clock.** The Mac sends over BLE/Wi-Fi and receives the same message back through the AudioFuse's
  USB MIDI port (and the other way round), so latency and jitter are measured one way, with one clock, by a script.
  No round trip to halve, no second board to debug.
- **TRS type mismatch:** the AudioFuse Studio's MIDI jacks are **TRS Type B**, the receiver is **Type A** (tip and ring
  swapped). Two A↔B crossover cables or adapters are needed (or the AudioFuse's included B->DIN adapters + Type A->DIN
  adapters + DIN cables). Plugged straight together nothing is damaged (the reverse diode at each opto clamps), but no
  data gets through.
- **Jacks on the receiver** (front wall, facing it, switch on the right): the right-hand jack, nearer the switch, is
  **J3 MIDI IN**; the one nearer the middle is **J2 MIDI OUT**.
- **MIDI-CI device stand-in:** the AudioFuse does not implement MIDI-CI, so a Mac program on the AudioFuse ports plays
  the device: it links the same `midici_responder` C library the devices will use, compiled for macOS, and answers
  Discovery and Property Exchange with a demo `ChCtrlList`. Sound checks: a soft synth on the Mac listening to the
  AudioFuse MIDI in (the user listens).
- Without the AudioFuse, the receiver still tests itself with a TRS cable from J2 to J3 (loopback).
- Toolchains: ESP-IDF v5.5.x at `~/Developer/esp-idf`, Python (bleak, python-rtmidi) for the test scripts. Mac:
  macOS 26.5.2.
- The Daisy Pod (libDaisy at `$LIBDAISY_PATH`, J-Link clone + OpenOCD) stays an option for a hardware demo device
  in phase 5; it has MIDI in only and would need a MIDI out added on D29 (PB14, USART1 TX; D13 is its encoder click).

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
  `ResourceList`/`DeviceInfo`/`ChCtrlList` gets, `ChCtrlList` driven from a static table). The Mac device stand-in
  uses it first, then the user's own devices.
- Specs (downloaded to `modeling/reference/`, not redistributable, referenced by number): M2-100 overview, M2-101
  MIDI-CI 1.2.1, M2-102 Profiles, M2-103 Property Exchange common rules, M2-104 UMP + MIDI 2.0 protocol 1.1.2,
  M2-117 PE Controller Resources 1.0.1, M2-124 Network MIDI 2.0 (UDP) 1.0.1.

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
  devices/mac_standin/       MIDI-CI device stand-in for the Mac (midici_responder on CoreMIDI), test-only
  devices/pod_synth/         optional hardware demo device (libDaisy, midici_responder)
  tools/                     hil/ (Python: bleak, python-rtmidi, serial), sign/, release helpers
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
  - Mac -> BLE/Wi-Fi -> receiver -> TRS -> AudioFuse -> USB -> Mac: every message arrives, in order, unchanged
    (notes, CCs, clock, long SysEx, bursts).
  - Mac -> AudioFuse -> TRS -> receiver -> BLE/Wi-Fi -> Mac: the same the other way.
  - One-way latency and jitter per transport and direction, timed on the Mac's clock, with Wi-Fi off and on,
    reported in `docs/measurements.md`.
  - Merge: both directions busy at once; the TRS out stays well-formed.
  - MIDI-CI: the Mac device stand-in on the AudioFuse ports answers Discovery + PE, and its `ChCtrlList` shows up in
    the web UI's JSON endpoint.
  - OTA: upload, self-test, mark valid; and a deliberately broken image rolls back.
- **The user's ears:** a soft synth on the Mac fed by the AudioFuse MIDI in, for the sound checks.

## Order of work

### Spikes first (throwaway code in `spikes/`, deleted or folded in later; results go into the decision log)

- **S0 rig:** A↔B crossover cables in place; a minimal receiver program echoes TRS in to TRS out; a Mac script
  sends through the AudioFuse out and reads it back on the AudioFuse in (message count, order, timing).
- **S1 Network MIDI on the Mac:** does macOS 26.5 find a Network MIDI 2.0 endpoint the ESP32 advertises (Audio MIDI
  Setup, a CoreMIDI client)? CoreMIDI has no public Network MIDI 2.0 API, only `MIDINetworkSession` (RTP-MIDI). If
  it does not: RTP-MIDI first, Network MIDI 2.0 later for Windows and future macOS.
- **S2 radio sharing on the C6:** BLE MIDI latency/jitter with Wi-Fi off, Wi-Fi connected but idle, and Wi-Fi busy.
  Decides how Network mode treats BLE.
- **S3 MIDI-CI over TRS:** receiver sends Discovery + one PE get; a Mac script on the AudioFuse ports answers from a
  hard-coded reply. Also: does macOS `MIDICIDeviceManager` discover a CI device through the receiver's BLE MIDI?

### Phases (each ends working, tested and committed)

1. **Foundation:** repo skeleton, CI (build both boards, host tests, format check), board layer, `lib/` UMP +
   MIDI 1.0 parser/encoder + translation with tests, logging, version string, NVS config.
2. **Bridge:** TRS in/out, BLE MIDI peripheral, router + merge, LED + button + modes (Play only), battery gauge +
   low-battery shutdown, light sleep; HIL test through the AudioFuse; power and latency measured.
3. **Wi-Fi + OTA:** Setup AP + station join, mDNS, web UI shell (status, config, update), partition table, signed OTA
   with rollback and self-test, GitHub Release builds.
4. **Network MIDI:** Network MIDI 2.0 or RTP-MIDI per S1 (the other one later), UMP endpoint + function blocks,
   WebSocket control surface (generic page).
5. **MIDI-CI:** initiator over TRS, PE gets, profile cache, pass-through, hand-written profile upload, generated
   control page; `midici_responder` library and the Mac device stand-in using it. Demo video: the web page discovers
   the device, draws its controls, a fader moves the synth's filter.
6. **Extras, in any order:** USB MIDI on the S3, ESP-NOW cable between two boxes, BLE central, the second network
   transport, OTA over BLE.

### Demo device (optional, phase 5)

The Mac stand-in proves MIDI-CI works; a real box is the better demo. Candidate: the Daisy Pod with a MIDI out added
(D29), running one voice (saw/square oscillator, low-pass filter with resonance, ADSR), about six CC parameters (cutoff, resonance,
attack, decay, release, waveform) described in its `ChCtrlList`. The Pod's knobs control cutoff and resonance and also
send their CCs out of the new MIDI out; the encoder picks the waveform; the LEDs show MIDI in and CI activity.
Oscillator and filter from DaisySP (to install next to libDaisy) or written by hand, whichever is smaller. Later the
user's own devices (sd_synth) take this role.

## Open questions

- ~~License~~: decided 2026-10-09, MIT for the code, CERN-OHL-P-2.0 for `hardware/` (decision log).
- Which S3 board gets mounted (second rev A board?), and when. Until then an ESP32-S3-DevKitC-1 is the second box
  (`build.sh s3kit`).
- The dev kit's Wi-Fi works only up to ~15 dBm on USB power; does a XIAO S3 on the receiver's TPS63020 need a limit?
- ~~Network MIDI 2.0 vs RTP-MIDI first: spike S1.~~ Network MIDI 2.0 (decision log 2026-10-07).
- ~~BLE + Wi-Fi together on the C6: spike S2.~~ Works, BLE stays on in Network mode (decision log 2026-10-07).
- ~~USB MIDI 2.0 class support in TinyUSB on the S3: check before phase 6.~~ USB MIDI 1.0 (decision log 2026-10-08).
- ~~A↔B crossover cables or adapters for the AudioFuse (Type B): what does the user have?~~ In place (S0).
- GitHub: repo created under the user's account when they say so; nothing is pushed until then.
- ~~ROM boot banner on MIDI OUT at every reset~~: eFuse burned on the C6 (decision log 2026-10-08); the S3 needs it too.

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
| 2026-10-07 | AudioFuse Studio replaces the Pod as the test rig; Pod only as an optional demo device | both ends on the Mac's clock, true one-way latency, no second firmware to debug; needs A↔B cables (AudioFuse is Type B) |
| 2026-10-07 | S0 passed: both TRS directions through the crossovers, every message back unchanged (notes, CCs on 16 channels, clock, 253-byte SysEx, 300-note burst) | round trip Mac -> AudioFuse -> receiver echo -> AudioFuse -> Mac: 3.0 ms median for 3-byte messages (1.9 ms of it is wire time at 31250 baud), 1.6 ms for clock; lesson: `uart_read_bytes()` waits for its full length, so read 1 byte then what is buffered |
| 2026-10-06 | Spikes S0-S3 before phase 1 | test what is uncertain before building on it |
| 2026-10-07 | S1 (static part): macOS 26.5 has Network MIDI 2.0 in its network driver (`NetworkMIDI2Session`, host and client roles, `_midi2._udp`, no auth); no public API, so sessions are set up in Audio MIDI Setup | strings in `AppleMIDINetworkDriver.plugin`; the live test (ESP32 advertising, Mac connecting) waits for Wi-Fi credentials |
| 2026-10-07 | S3 passed (TRS part): Discovery, PE capabilities and Get ResourceList / DeviceInfo / AllCtrlList over TRS in 0.55-0.72 s; a 788-byte control list takes 320 ms (2.7 KB/s of the 3.1 KB/s line rate) | faster than the 1-2 s guessed; the flash cache stays, but a rescan at every boot is affordable |
| 2026-10-07 | `AllCtrlList` instead of `ChCtrlList` as the control list resource | M2-117: `ChCtrlList` only exists behind a `ChannelList` entry (needs `resId`); `AllCtrlList` is the device-wide list and the one M2-103's own example uses |
| 2026-10-07 | macOS sends MIDI-CI Discovery on hardware MIDI ports by itself (seen arriving at J3 during S3) | the receiver will see Discovery messages it did not start; the CI layer must ignore or forward them by MUID, never treat them as replies |
| 2026-10-07 | S2 passed: BLE MIDI and Wi-Fi share the C6 radio without loss. Through CoreMIDI's BLE driver (7.5 ms interval): BLE -> TRS 15.7 ms median / 19 ms p95, TRS -> BLE 9.1 / 12.5 ms with Wi-Fi off; Wi-Fi joined and idle changes little (16.2 / 24.8 and 8.9 / 16.1); 100 KB/s to 700 KB/s of UDP raises p95 to ~40 ms (median +3-5 ms); 0 lost in every mode | Network mode keeps BLE on; Play mode keeps Wi-Fi off for the tightest timing; no bulk transfers (OTA) while playing |
| 2026-10-07 | BLE MIDI tests go through CoreMIDI, not bleak | from CoreBluetooth directly macOS keeps a 30 ms interval (20 ms median, 34 ms p95); Apple's MIDI driver sets 7.5 ms. `tools/hil/blemidi` hands the device to CoreMIDI (`MIDIBluetoothDriverActivateAllConnections`) |
| 2026-10-07 | HIL order: connect BLE first, then open other CoreMIDI ports | while another process holds a CoreMIDI client, the activation is ignored, and the first activation after that (or after `MIDIBluetoothDriverDisconnect`) fails too; `blemidi connect` is retried |
| 2026-10-07 | Connection parameter request: 11.25-26.25 ms | Apple's accessory guidelines (min >= 11.25 ms for MIDI, max >= min + 15 ms); macOS's MIDI driver then picks 7.5 ms itself |
| 2026-10-07 | S3 passed (BLE part): macOS runs MIDI-CI Discovery on a BLE MIDI 1.0 endpoint about 2 s after connecting, and `MIDICIDeviceManager` lists the responder (type legacy MIDI 1.0) | pass-through of CI between the Mac and the TRS device is worth doing; the device must answer within macOS's discovery window, which the 31250 baud round trip (~60 ms for Discovery) allows |
| 2026-10-07 | S1 passed: macOS 26.5 finds an ESP32 Network MIDI 2.0 host over mDNS, connects from Audio MIDI Setup (MIDI 2.0 protocol session, no auth) and passes MIDI both ways; 6.4 ms out / 7.1 ms in median, p95 60-78 ms from the Wi-Fi link itself (plain ping to the receiver: max 104 ms) | Network MIDI 2.0 first, RTP-MIDI later (Windows rtpMIDI, older macOS) |
| 2026-10-07 | Network MIDI needs Retransmit Request (and FEC) from the start | at 5 ms spacing 45/600 UDP packets were lost on air with no recovery in the spike; macOS's driver supports retransmit |
| 2026-10-07 | The NM2 session config is a CoreMIDI device property (`midi2.udp.session` with `peers` on the UMP Network device's entity) | lets the HIL set up the session without clicking in Audio MIDI Setup (to try in phase 4) |
| 2026-10-07 | Phase 1 done: lib/ (UMP, MIDI 1.0 parser/encoder, M2-104 translation, settings rules) with host tests under ASan/UBSan, firmware for both boards, CI workflow, clang-format pinned to 18.1.8 | CI has not run on GitHub yet (nothing pushed); `tools/check.sh` runs the same steps locally |
| 2026-10-08 | One bus task owns routing and every output; transports only `bus_send()` (non-blocking) | a slow output can never stall a radio stack; port up/down also travels through the queue so only one task touches the merges |
| 2026-10-08 | BLE MIDI timestamps are ignored on receive (send as soon as it arrives) | lowest latency; timestamp scheduling would trade the 7.5 ms interval jitter for a fixed delay of the same size |
| 2026-10-08 | Merge per MIDI 1.0 output: real-time first, a SysEx holds other messages (each source's order kept) until it ends or 500 ms pass | the randomized test found the order bug in the first version (a note off could overtake its note on) |
| 2026-10-08 | Battery: under 2.5 V on A0 means "switch off, on USB", never "empty" | the first firmware slept 5 s after boot on USB power because A0 sits after the switch; needed BOOT + a replug to recover |
| 2026-10-08 | A second BLE source for the merge test comes from a firmware test hook (console `m`), not a second central | a second CoreBluetooth client on the same Mac shares CoreMIDI's link to the receiver |
| 2026-10-08 | Phase 2 passed on the hardware: `bridge_test.py` 15/15 twice (BLE -> TRS 8.4 ms median, TRS -> BLE 9.5 ms at a 7.5 ms interval); its first run found 4 real bugs (receive buffer, notification retries, UART buffer size, draining) | see docs/design/transports.md |
| 2026-10-08 | Boot with frequency scaling (40-160 MHz), no automatic light sleep | light sleep is the only state the USB meter shows as clearly lower, but it lost 6/15 TRS messages after idle and silences the USB console; LP UART on rev B would fix it (docs/measurements.md) |
| 2026-10-08 | CoreMIDI-path tests need the Mac's MIDI driver to cooperate; correctness runs over bleak | the driver stopped taking the receiver after the spike's disconnect tests and Audio MIDI Setup (docs/design/testing.md) |
| 2026-10-08 | Signed images with RSA-3072 (Secure Boot V2 format) on both boards, checked on update, no eFuses | the S3 has no ECDSA variant; hardware Secure Boot stays off on the only C6 (docs/design/ota.md) |
| 2026-10-08 | `ota_end()` erases the sector after the image before verifying | ota_test.py found an unsigned image accepted against a signature left by the previous upload |
| 2026-10-08 | Web UI in the forslike style, plain JS, polling /api/status at 1 Hz; open AP in Setup mode | no app to install, 18 KB gzipped; the AP only exists after an 8 s press and accepts only signed firmware |
| 2026-10-08 | Network mode runs without BLE by default (setting `ble_in_network`), Wi-Fi power save off | replaces 2026-10-07 "Network mode keeps BLE on": S2 measured BLE's side only; with the BLE stack running network MIDI took 22 ms median, without it 6.7 / 4.2 ms (docs/design/network-midi.md) |
| 2026-10-08 | Network MIDI: FEC 2, retransmit, hold-don't-reorder with a 150 ms give-up; never drop a client for unanswered pings | 0/600 lost at 5 ms spacing (spike: 45); macOS 26.5 does not answer pings and sends nothing while idle (WRONG, see the session resumption row: an artifact of nm2connect) |
| 2026-10-08 | One function block for the whole box (per-port blocks later) | per-port blocks need group-aware routing; macOS works with one |
| 2026-10-08 | The box's MIDI-CI initiator is a bus port (`PORT_CI`) routed to and from TRS only, SysEx only; pass-through is ordinary routing | the box's Discovery stays on the TRS link; a CI host behind the box reaches the device with no extra code (receivers ignore other MUIDs); tested with a host on the WebSocket |
| 2026-10-08 | Initiator: one request at a time, first device with PE wins, ResourceList decides what else to get (DeviceInfo, AllCtrlList, one CtrlMapList per ctrlMapId) | every responder supports one request; a MIDI 1.0 cable has one device; the page needs the value names of valueSelect controls |
| 2026-10-08 | No JSON parser on the box: a key scanner for the 4 fields it needs, cJSON only to validate what is stored, the browser parses the rest | small, and the API can splice stored JSON into its answers unchanged |
| 2026-10-08 | Cache one device in LittleFS keyed by manufacturer/family/model/version; hand-written profile as fallback | a known device rescans in ~50 ms instead of ~0.8 s and hears only a Discovery; one device never fills the partition |
| 2026-10-08 | The page sends the device's controls as MIDI 2.0 (32-bit) and lets the box scale them for the jack | the control list's values are 32-bit (default, minMax, CtrlMapList values); one translation, already tested |
| 2026-10-08 | Web UI WebSocket: one queued send at a time | per-batch async sends overflowed the server's 6-slot work queue under SysEx from TRS and lost frames (found by the pass-through test) |
| 2026-10-08 | Phase 5 done (MIDI-CI over TRS, cache, profile, generated play page, responder library, Mac stand-in): host tests 12/12 suites, `ci_test.py` 21/21, page -> device and device -> page checked in a browser | macOS's MIDICIDeviceManager found nothing this time (no Discovery from macOS), so pass-through is tested from the WebSocket; the Daisy Pod demo device is still optional |
| 2026-10-08 | OPEN: the chip's ROM boot banner leaves through MIDI OUT (D6 = UART0 TX on both XIAOs) at every reset, ~8 random messages incl. Program Changes | firmware cannot stop it; burn `UART_PRINT_CONTROL` (irreversible, harmless) or move MIDI OUT to another GPIO on rev B: the user decides (docs/design/midi-ci.md) |
| 2026-10-08 | Burned `UART_PRINT_CONTROL` = 0b11 on board #1's C6 (user's decision) | resets no longer send garbage on MIDI OUT (0 messages in 4 hardware resets, software restarts only all-notes-off); download mode still leaks 3 messages (bench only); the S3 needs the same when it is mounted |
| 2026-10-08 | Network MIDI 2.0: the box takes back sessions after a restart (Session Reset instead of Bye 0x05) and resumes the last client's session when it joins Wi-Fi | macOS never invites a host again after a session ends (Bye from either side or ping timeout), so every restart needed a manual reconnect; resume_test.py: back in 4.7-5.3 s after a reset, 20/20 each way, 3 of 3. Deviates from M2-124 on purpose (docs/design/network-midi.md) |
| 2026-10-08 | macOS 26.5 answers pings and pings every 60 s; still never drop a client for unanswered pings | the phase 4 "no pings" finding came from nm2connect writing the peer without `authentication-data`; the rule stays because ending a session loses macOS for good |
| 2026-10-08 | lwIP sockets 10 -> 16 | one browser tab exhausted the 10 (web server 7 + listen + control + Network MIDI) and accept() failed before the server could drop an old connection |
| 2026-10-08 | The box announces its UMP endpoint when a Network MIDI 2.0 session starts (Endpoint Info, identity, name, instance id, stream configuration), once the client has spoken; the function block declares MIDI-CI | macOS never sends Endpoint or Function Block Discovery over the session, so it knew nothing of the box and ran no MIDI-CI: the reason `MIDICIDeviceManager` found nothing in phase 5. Told, macOS asks for the block, switches to MIDI 2.0 and runs Discovery through the box (`mac_ci_test.py` 12/12). Announcing right after Invitation Accepted got `Bye 0x05` in 1 of 4 connects; after the client's first packet, 8 of 8 |
| 2026-10-08 | Guard each MIDI 1.0 output against MIDI-CI Discovery loops: a Discovery from another port within 1 s of the last one written is dropped (`lib/ci_guard`) | macOS passes Discovery from one port on to its other MIDI-CI endpoints; with TRS cabled back to the same Mac every Discovery circled until the TRS line was full (120 000 messages, scans timing out). Loops die on their first lap now |
| 2026-10-08 | `latency.py` skips CC 0, 6, 32, 38, 98-101 | in a MIDI 2.0 protocol session those are not passed on as plain CCs (M2-104), which read as 32/300 lost; with them skipped 0/300 and 0/600, medians within 1 ms of phase 4 |
| 2026-10-08 | RTP-MIDI as the second network transport, both run in Network mode; recovery journal for chapters P, C, W, N, T, closed-loop checkpoint, history for 4 channels at once | the journal repairs losses without retransmission (RFC 6295's mandate, tested as a property through 30-50% loss); 16 channels of history cost 26 KB of the C6's heap |
| 2026-10-08 | RTP-MIDI packets with the M bit 0, against RFC 6295 | macOS 26 held packets with M = 1 back ~25 ms and dropped some (86/300); Apple's own packets have it 0; with 0: 4-6 ms, none lost (docs/design/rtp-midi.md) |
| 2026-10-08 | RTP-MIDI sessions are taken back after a restart: SSRC from the MAC address, session clock from SNTP wall-clock time, sequence numbers found with two probes half the range apart | macOS never re-invites a box that went away; a clock restarted from 0 looks days ahead in 32-bit RTP time and macOS holds such MIDI; macOS drops packets "behind" its last. rtp_test.py: 5/5 resets taken back |
| 2026-10-08 | BLE central: one device (`ble_device`: none, any, a name or an address), "just works" pairing when asked, connection updates held to 15 ms | the box can take a BLE keyboard by itself; macOS's peripheral advertises no name, so an address works too. central_test.py 14/14, Play mode 13.0 / 11.3 ms median |
| 2026-10-08 | Firmware update over BLE (ble_update): the same signed images, offsets in every write and go-back-N | for boxes not on Wi-Fi; CoreBluetooth drops writes without response it has no room for and bleak 3.0 does not wait. 1.6 MB in 69 s, unsigned and corrupted images refused (ble_ota_test.py 9/9) |
| 2026-10-08 | USB MIDI 1.0 (TinyUSB) on the S3, console on a CDC port beside it; S3 console no longer USB-Serial-JTAG | every host speaks 1.0; TinyUSB takes the PHY, and writes to USB-Serial-JTAG afterwards (a panic, the next boot's log) hung the chip. usb_test.py 162/162 each way |
| 2026-10-08 | ESP-NOW cable: hellos per group, unicast with radio-level retries, duplicates dropped; a free box looks for its peer, back to channel 1 every other step | two boxes as a wireless MIDI cable with no access point, one may be on an access point's channel. espnow_test.py 9/9, 3.5-4.1 ms median including TRS and USB, 0 lost |
| 2026-10-08 | ESP32-S3-DevKitC-1 as the second box (`build.sh s3kit`), its Wi-Fi held to 15 dBm | the S3 receiver is not mounted yet; at 20 dBm none of the kit's Wi-Fi frames arrived (no ESP-NOW acknowledgement, no access point answer) while BLE worked, at 10-15 dBm all did |
| 2026-10-08 | Network MIDI starts with Network mode; BLE backlogs come with their connection; PSRAM on the S3 | the S3 had 7 KB of heap in Play mode; ~45 KB of session state was idle there |
| 2026-10-08 | Phase 6 done: RTP-MIDI, BLE central, OTA over BLE, USB MIDI, ESP-NOW cable, each tested on the hardware | host tests 17/17 suites; rtp_test, central_test, ble_ota_test, usb_test, espnow_test all pass, and the earlier hardware tests still do |
| 2026-10-09 | Public on GitHub (TTeuber/AirMidi): MIT for code and docs; CI on ESP-IDF v5.5.5; first release v0.1.0 signed with the release key in CI | MIT is what embedded libraries (TinyUSB, Unity) use and asks nothing of a reader; v5.5.1 lacks the httpd callback the web UI uses |
| 2026-10-10 | `hardware/` = rev B, exported from the design workspace as outputs only (PDFs, renders, KiCad project with bundled libraries, Gerbers, BOM, STL/STEP), under CERN-OHL-P-2.0 | rev B fixes rev A's reversed cell connector; the generator code needs the whole workspace, so it stays there; LCSC 3D models left out (redistribution terms unclear) |

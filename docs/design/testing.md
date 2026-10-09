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
- `cidevices.swift`: what macOS's `MIDICIDeviceManager` discovered (MIDI-CI over BLE and through the box). It
  keeps devices that no longer answer, so every stand-in run adds an entry; match on the MUID.
- `mac_ci_test.py`: macOS finds the stand-in through the box over Network MIDI 2.0, and the box's own Discovery
  does not loop through the Mac (see [midi-ci.md](midi-ci.md), "Loops").
- `bleport.py`: a BLE MIDI connection straight from CoreBluetooth (bleak), shaped like an rtmidi port, for when
  CoreMIDI's driver will not take the receiver. It packs queued messages into MTU-sized packets like a real sender:
  one write per message made macOS drop most of a 300-note burst before it reached the air.
- `bridge_test.py`: the phase 2 acceptance test (see [transports.md](transports.md)); `--bleak` uses bleport.
- `ci_test.py`: the phase 5 acceptance test against `devices/mac_standin` (a MIDI-CI device on the AudioFuse
  ports, `lib/midici_responder` on CoreMIDI); see [midi-ci.md](midi-ci.md).
- `ws_test.py`: the web page's WebSocket both ways through the AudioFuse.
- `resume_test.py`: connect Network MIDI once, reset the box 3 times, MIDI must come back both ways each time with
  nobody touching Audio MIDI Setup (see [network-midi.md](network-midi.md)).
- `nm2connect.swift`: connects or disconnects (`--remove`) macOS's Network MIDI 2.0 session to the box.
- `rtpconnect.swift`: the same for one of macOS's RTP-MIDI sessions (made once in Audio MIDI Setup), through its
  `apple.midirtp.session` property; `MIDINetworkSession` stays disabled from a command-line tool on macOS 26.
- `rtp_test.py`: RTP-MIDI with macOS's own driver, both ways at 20 and 5 ms, then resets: the box must take the
  session back each time ([rtp-midi.md](rtp-midi.md)). It keeps the session's port open: macOS closes a session
  nobody listens to.
- `blepeer.swift`: a BLE MIDI peripheral on the Mac (CoreBluetooth), standing in for a keyboard the box connects to;
  `central_test.py` runs the box's BLE central against it, in Network and Play mode.
- `ble_ota.py`, `ble_ota_test.py`: firmware update over BLE (bleak), refused and accepted images.
- `usb_test.py`: USB MIDI on an S3 box, with BLE MIDI on the other side, so a bare dev kit is enough.
- `espnow_test.py`: the ESP-NOW cable between the C6 (TRS, Network mode) and an S3 (USB MIDI, Play mode).

Run `resume_test.py` on its own: straight after `ci_test.py` in one chain, its first invitation went unanswered
once (alone, right after: 10/10). Not pinned down; CoreMIDI has shown other order effects (see BLE above).

A second box: the S3 tests run on an ESP32-S3-DevKitC-1 (`firmware/build.sh s3kit`). Over its native USB port it is
a USB MIDI device with its console beside it; `build.sh s3kit flash` gets it into its bootloader through that port
([transports.md](transports.md), USB MIDI). Its UART port, when connected, shows the boot log and panics.

CoreMIDI's Bluetooth driver turned out fragile under test automation: after a few `MIDIBluetoothDriverDisconnect`
calls and with Audio MIDI Setup open for a while, it stopped promoting this peripheral at all (`bluetoothd` lists
`com.apple.MIDIServer` as a client of the device, the server never acts on the activation), and quitting Audio MIDI
Setup or deleting the stale device did not bring it back. So correctness tests run over bleak, and CoreMIDI-path
latency is measured when the driver cooperates.

Spikes S0-S3 (`spikes/`) used these first; their results are in the plan's decision log.

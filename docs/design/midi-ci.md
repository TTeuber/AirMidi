# MIDI-CI (`lib/midici*`, `lib/json_scan`, `firmware/components/ci`, `devices/mac_standin`)

The box asks the device on its TRS jacks who it is and what it can be controlled with, and the web UI draws that
device's own controls. MIDI-CI (M2-101) runs over SysEx, so it works on a plain 31250-baud MIDI 1.0 cable; the
control list is a Property Exchange (M2-103) resource defined by M2-117. Devices without MIDI-CI (almost every
synth on the market) get the same page from a hand-written profile.

## Layers

- `lib/midici`: building and parsing the messages: Discovery and its reply, PE Capabilities, Get Property Data
  (with `resId`), ACK/NAK, Invalidate MUID. Messages are SysEx contents (between F0 and F7).
- `lib/midici_initiator`: the box's side, pure logic like `nm2` (messages and the time in, messages and results out
  through callbacks, no allocation). One scan:

  ```
  Discovery (broadcast) ──▶ first reply with the PE bit ──▶ on_device(): cached? stop here
  PE Capabilities ──▶ Get ResourceList ──▶ Get DeviceInfo, AllCtrlList (if listed)
                                      ──▶ Get CtrlMapList resId=<id> for each "ctrlMapId" in the AllCtrlList
  ```

  One request at a time (every responder supports one). Chunks must arrive in order; a gap fails that resource
  (status 342) and the scan goes on with the next. A silent device ends the scan after 3 s (M2-103 12.2), and an
  ACK "timeout wait" extends that. Discovery waits the full 3 s (M2-101 5.5.5) only if nobody with Property
  Exchange answers; on a MIDI 1.0 cable there is one device, so the first one with PE is taken at once.
- `lib/midici_responder`: the device side: Discovery reply, PE Capabilities, Get from a table of
  `(resource, resId, json)` with a generated ResourceList, replies chunked to each initiator's Receivable Maximum
  SysEx size, NAK for inquiries it does not support, never an answer to a reply (two devices cannot loop). Meant
  for a synth's firmware; `devices/mac_standin` uses it on CoreMIDI.
- `lib/json_scan`: the box needs four things out of JSON (a reply's `status`, a request's `resource`/`resId`, the
  names in a ResourceList, the `ctrlMapId`s of an AllCtrlList). A scanner that walks the text, skips string
  contents (escapes included) and reports every `"key": value` at any depth does that in 90 lines; the browser
  parses the real thing.
- `firmware/components/ci`: the bus port, the task, the cache and the profile; `/api/ci*` in `webui`.

## On the bus: PORT_CI

The initiator is a bus port of its own. The route matrix connects it to `PORT_TRS` only, and for SysEx only
(`router_init`): its Discovery never goes out on BLE or the network, and it never sees a note. Everything else
about TRS is unchanged, so the device's replies still reach every other port too: a MIDI-CI host behind the box
(macOS over BLE or Network MIDI, a page on the WebSocket) can talk to the device through it. That is the plan's
pass-through, and it costs no code: CI receivers ignore messages addressed to another MUID (M2-101 3.3), which is
also how the initiator ignores the replies meant for that host and the Discovery macOS sends on hardware ports by
itself (spike S3).

The bus task reassembles SysEx7 packets into whole messages (`sysex7_rx`) and hands MIDI-CI ones to the CI task
through a 4 KB FreeRTOS message buffer, without blocking; drops are counted (`dropped` in `/api/ci`). The CI task
sends by splitting into packets and `bus_send(PORT_CI, ...)`; the TRS merge keeps each SysEx whole.

## Cache and profile (LittleFS on the `storage` partition)

`/store/ci/` holds the last device's answers: `key.txt` (manufacturer-family-model-version from Discovery),
`info.json`, `controls.json`, `map-<ctrlMapId>.json`. When Discovery returns the same key, the scan stops there
and the cache is used: the device's identity changes whenever its MIDI implementation does (M2-101 5.5.1 requires a
new Software Revision Level then). One device only: swapping synths costs one full scan (under a second), and the
partition never fills. A short press, the console (`d`) and the page's "ask again" button (forced, `D`) rescan.

Only JSON that parses (cJSON, once, when it arrives) is kept, so `/api/ci/device` can splice the stored text into
its answer unchanged: `{"info": DeviceInfo, "controls": AllCtrlList, "maps": {id: CtrlMapList}, "profile": ...}`.

A hand-written profile (`POST /api/ci/profile`, 16 KB at most) is either a bare AllCtrlList or
`{"info": ..., "controls": [...], "maps": {...}}`; the page uses it when no MIDI-CI device answered.

## The page

The play tab draws the control list above the generic page: entries sorted by `priority`, priority 5 and
`"recognize": "none"` hidden; `typeHint` `toggle`/`momentary` become toggles, `valueSelect` with a CtrlMapList (or a
`stepCount` up to 8) a picker, everything else a fader whose value shows as the nearest named value at or below it
(a cutoff map names frequencies along the range) or as a percentage. Types: `cc`, `chPress`, `pBend`, `rpn`,
`nrpn`; the per-note types need a note and are listed as not shown. Controls send MIDI 2.0 channel voice with the
full 32-bit value; the box scales it down for the MIDI 1.0 jack (M2-104 rules, `lib/translate`). Incoming CCs
from the device move the controls.

Stepped values without a CtrlMapList are sent at `k * 2^32 / stepCount`, the spacing the stand-in's map uses; M2-117
points to the Bit Scaling and Resolution spec for this, which is not in `reference/` (to check).

## Measured (C6, TRS at 31250 baud, the stand-in on the AudioFuse)

| | |
|---|---|
| Full scan: Discovery, capabilities, 4 resources (98 + 174 + 989 + 105 bytes of JSON) | 720-910 ms |
| Rescan of a known device (Discovery only, then the cache) | 37-74 ms |
| Scan with nothing on the jacks | 3.0 s (the spec's Discovery timeout) |
| A 512-byte chunk on the wire | 164 ms |

Spike S3 measured 0.55-0.72 s for the same exchange with a smaller list; the decision to cache stays because the
rescan at boot is then silent for the device, not because the scan is slow.

## Tests

- Host (`test/test_midici.c`, 21 tests): JSON scanning (keys inside strings, escapes, truncation), message layouts
  against hand-computed bytes, and the initiator against the responder over a simulated link: full scan with
  chunking checked against the 512-byte limit, cached device, nobody answering, a device without PE, strays
  (macOS's Discovery, replies to another MUID, our own echo), a lost chunk, a silent device, the timeout-wait ACK,
  Invalidate MUID, a reply larger than the buffer, repeated scans, the responder's 404/NAK and 128-byte chunking,
  and 20000 fuzzed messages under ASan/UBSan. Mutation check: 7 of 8 mutants killed; the survivor (appending
  SysEx data while no message is open) cannot change the output.
- Hardware (`tools/hil/ci_test.py`, 21 checks): scan, the served JSON equal to what the stand-in holds, the cached
  rescan sends no Get, page fader -> device CC at 0/64/127, a MIDI-CI host on the WebSocket reads the 989-byte
  AllCtrlList through the box, profiles, "no device" when the stand-in is gone.

## What the hardware found

- **WebSocket frames lost under SysEx** (fixed): the web output queued one async send per bus batch; SysEx from
  TRS makes a batch every ~2 ms, the HTTP server's work queue is a UDP socket with a 6-message mailbox, and a
  failed queue freed the frame silently. Phase 4's test (CCs 20 ms apart) never got there; pass-through did (3 of
  6 packets of a Discovery reply arrived). Now one send is queued at a time and takes everything collected; what
  does not fit is counted.
- **The ROM boot log came out of MIDI OUT** (fixed on board #1 by an eFuse): the XIAO's D6 (GPIO16 on the C6,
  GPIO43 on the S3) is UART0 TX, where the chip's ROM prints its boot banner at 115200 baud before any firmware
  runs. Read at 31250 baud by the device, each reset sent ~8 random messages, among them Program Changes (seen:
  channels 1, 5, 12) that would switch a connected synth's patch. Firmware cannot stop it. On 2026-10-08 the C6's
  `UART_PRINT_CONTROL` eFuse was burned to `0b11` (ROM output on UART0 off for good; the USB console keeps it,
  that is `DIS_USB_SERIAL_JTAG_ROM_PRINT`, untouched). Checked with the stand-in on MIDI OUT: hardware resets
  0 messages (4 of 4 after the first boot following the burn), software restarts only the intended all-notes-off.
  Entering download mode (esptool, flashing) still sends 3: that is the ROM's download handler, a bench-only
  path. The S3 has the same pin and needs the same fuse (or rev B moves MIDI OUT to another GPIO).
- One boot scan out of five found no device although the stand-in answered it (its log shows the reply to the
  box's MUID). Not reproduced in the next five resets; the next manual or button scan works. Left as observed.
- macOS's `MIDICIDeviceManager` discovered nothing during this phase, not even the stand-in on the AudioFuse
  directly, and sent no Discovery on its own; in spike S3 it did. So pass-through is tested with a host on the
  WebSocket instead of macOS.

## Not done

Subscriptions (live updates of a resource), Set Property Data, Profiles, Process Inquiry; MIDI-CI over BLE or the
network from the box itself (the box only asks the TRS device); per-note controls on the page.

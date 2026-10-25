# airmidi app: plan

Written 2026-10-24, before any code. An Android app (Flutter, so iPad later is possible) that is the easy way to set
up and play the airmidi box from a phone, and the home of the pedal steel strips from `modeling/proto/pedal_steel`.
The web UI stays: it is the fallback when there is no app, and the browser on a laptop.

## Goals

1. **Setup without the browser dance.** Find the box, give it Wi-Fi, pick a mode, from one screen. No joining the
   box's access point, no typing 192.168.4.1.
2. **Connect over BLE or Wi-Fi** with the same screens, whichever the box is on.
3. **Strips:** the pedal steel prototype as a control surface, sending MPE to whatever synth sits behind the box.
4. Later: the MIDI-CI control page (native), firmware updates pushed from the phone, the box's other settings.

Out of scope for now: an on-phone synth, the Play Store, iOS builds.

## What the box offers today (firmware v0.1.0)

| | Play mode | Network mode | Setup mode |
|---|---|---|---|
| BLE MIDI (standard service) | yes | only with `ble_in_network` (off by default: costs network MIDI ~16 ms) | yes |
| BLE update service (custom, `ble_update`) | yes | as above | yes |
| HTTP `/api/*` + WebSocket `/ws` (UMP, binary) | no (Wi-Fi off) | yes, `airmidi-xxxx.local`, mDNS `_http._tcp` | yes, open AP, 192.168.4.1 |

Gaps for the app:
- **No settings over BLE.** Status, settings, Wi-Fi and mode exist only as HTTP, so in Play mode a phone can play
  but not configure.
- **Setup mode is AP only.** The box cannot test a network while it runs its AP, and there is no scan endpoint, so
  the user types an SSID blind (and could pick a 5 GHz network the 2.4 GHz-only radio never sees).
- No stable id shared by the BLE advertisement and mDNS to tell that two sightings are the same box (the name is
  close, but the user can rename it).

## Platform choice: Flutter

Recommendation: Flutter, Android first. React Native was the other option.

- **Touch to radio stays in one thread and one language.** Flutter's `Listener` delivers raw pointer events (id,
  position) straight into Dart; the strips logic, the painter and the BLE write all run there. In React Native, smooth
  multi-touch means gesture-handler worklets on the UI thread, Skia for drawing, and a hop to the JS thread for every
  BLE write (react-native-ble-plx passes values as base64 over the bridge). It can be made to work, but it has more
  parts to tune for the one feature where latency matters.
- **BLE plugin:** `flutter_blue_plus` covers what we need on Android: MTU request, connection priority (short
  interval), write without response, notifications, bonding.
- **Reuse is small either way.** `strips.js` is about 200 lines of pure logic with tests: porting it to Dart is an
  afternoon, and its tests port with it. The web UI's control page gets rewritten in either framework.
- Flutter 3.27.1 is already installed (upgrade to the current stable first); the Android SDK and adb are there.
- Native Kotlin would give the best latency, but locks out the iPad. The spike (A0) measures whether Flutter is fast
  enough, before anything else is built on it.

## Architecture

```
 app (Flutter)                                                   box (firmware)
 ┌──────────────────────────────────────────┐
 │ screens: devices, setup, status, settings,│
 │          strips, controls, update         │
 │            │                 │            │
 │   BoxLink (one interface)    MidiOut      │
 │   status/settings/wifi/mode  (MPE, packed │
 │            │                 per interval)│
 │   ┌────────┴───────┐   ┌─────┴──────┐     │
 │   │ BleLink        │   │ WifiLink   │     │
 │   │ config service │   │ HTTP /api  │──────▶ webui (HTTP + /ws)    ┐
 │   │ BLE MIDI char  │   │ /ws UMP    │     │                         ├─ api: one set of handlers
 │   └────────┬───────┘   └────────────┘     │                         │  (method, path, JSON body)
 └────────────┼──────────────────────────────┘                         │
              └──────────────────────────────▶ ble_config (new GATT)    ┘
                                              ble_midi (unchanged)  ─▶ bus ─▶ TRS / USB / network
```

- **One API, two transports.** The firmware's HTTP handlers move into an `api` component (`api_call(method, path,
  body) -> status + JSON`), called by the web server and by a new BLE config service. The app's `BoxLink` has the same
  calls over HTTP or BLE, so every screen works on either.
- **BLE config service** (custom 128-bit UUIDs next to `ble_update`'s): a request characteristic (write) and a
  response characteristic (notify), each message a small header (request id, total length, offset) plus a JSON
  chunk, so a reply bigger than the MTU arrives in pieces. The app asks for a 517-byte MTU; Android usually gets 247+.
- **Security:** the config characteristics need an encrypted link (bonded, "just works", as the BLE central already
  does). New bonds are accepted only in a pairing window: Setup mode, or a few minutes after the BLE button hold (rev
  B: "hold = pair"). Bonded phones can configure at any time. BLE MIDI itself stays open, as now. Today the LAN web
  UI and the Setup AP are open, so this is stricter than what exists; the HTTP side can follow later.
- **Discovery:** BLE scan (BLE MIDI service UUID or the config service UUID in the advertisement) merged with mDNS
  (`_http._tcp`, Android NSD via `bonsoir` or similar). A TXT record `id=<MAC suffix>` on mDNS and the same id in
  the BLE scan response let the app show one box with two transports.
- **Transport choice:** in Play mode the app uses BLE for everything. In Network mode it uses Wi-Fi (HTTP + `/ws`)
  when the phone is on the same network, BLE if `ble_in_network` is on. Setup mode: BLE.

## Setup flow (the "easy" part)

1. Box is new or in Setup mode (STP button / 8 s hold). App: **Add box** lists boxes over BLE with signal strength.
2. Tap one: Android shows its pairing prompt once ("Pair with airmidi-7530?"), the app bonds.
3. The box scans and the app lists the networks the box itself can see (2.4 GHz only, strongest first). Pick one,
   type the password.
4. The box tries the network while staying in Setup mode and replies: joined (with IP) or the failure reason it
   already logs (wrong password, not found, no IP).
5. App offers: **Network mode** (Wi-Fi MIDI, web UI) or **Play mode** (BLE + TRS). The box switches; the app follows
   it over the matching transport.

Wi-Fi can be added or removed later from Settings in any mode, over BLE. Fallback when BLE fails: the app joins the
box's AP itself (Android `WifiNetworkSpecifier`, one system dialog, then the app's traffic is bound to that network)
and runs the same screens over HTTP. Only built if BLE setup turns out unreliable.

## Strips over MIDI (MPE)

- Strips map to MPE member channels: manager channel 1, strip *n* on channel 1+*n*. On connect the app sends the MPE
  configuration (RPN 6) and pitch bend range ±48 semitones (RPN 0) per member channel. 14 bits over 96 semitones is
  0.6 cents per step.
- Touch down: pitch bend for the offset from the nearest note, then note on. Moving: pitch bend only. Release: note
  off. A second finger on a sounding strip (legato) moves the bend, or retriggers when legato is off, as in the
  prototype. Pitch assist runs in the app, so the box and synth just see bends.
- **Pack per connection interval.** A send loop at the BLE connection interval (7.5-15 ms) writes one BLE MIDI packet
  holding the latest bend per strip and any note ons/offs, with timestamps. Bends between sends are coalesced to the
  newest, and a bend under 0.5 cents of change is skipped. That keeps the link from queueing, which is what makes
  slides lag.
- Over Wi-Fi the same messages go as MIDI 1.0 channel voice UMP over `/ws` (the box's existing format). MIDI 2.0
  per-note pitch (32-bit) comes later with UMP over TRS to daisy_synth (receiver rev C).
- **The synth must speak MPE.** daisy_synth has one global bend today (`synth.h`, `bend_`): it needs an MPE mode (a
  voice per member channel, per-channel bend, the bend range RPN). Until then, any MPE synth on the Mac works behind
  the box (its USB MIDI on the S3, Network MIDI, or TRS into the AudioFuse).

## Phases

- **A0 spike (go/no-go):** Flutter app with the strips ported (logic + tests + painter, no settings yet) writing BLE
  MIDI straight to the box's existing BLE MIDI characteristic; no firmware changes. Listen through a Mac MPE synth
  behind the box. Measures: touch-to-packet time in the app, the connection interval Android actually grants,
  dropped or bunched packets (box counters), and how a slide feels compared to the iPad prototype. Also sends the same
  over `/ws` in Network mode, to compare. Decides: Flutter stays (or Kotlin), BLE vs Wi-Fi for playing.
- **A1 firmware:** `api` component (handlers out of webui), `ble_config` service with chunking, bonding + pairing
  window, Wi-Fi scan endpoint, test-join in Setup mode (AP + station), `id` in mDNS TXT and the scan response.
  Host tests for the chunking; HIL test with bleak (`tools/hil/ble_config_test.py`), the existing web tests still
  pass.
- **A2 app:** devices screen (BLE + mDNS merged), bonding, setup flow, status, settings, mode switch, forget box.
- **A3 app:** strips as a full feature: the prototype's layout settings (row height, strip width, count, tunings,
  snap, legato, key guide, assist), presets saved per box, transport indicator, panic.
- **A4:** daisy_synth MPE mode, so the desktop kit plays the strips.
- **A5 app:** MIDI-CI control page drawn natively from `/api/ci/device`; firmware update: the box fetches it on
  Wi-Fi, or the phone downloads the signed release from GitHub and pushes it over `ble_update` in Play mode.

Status (2026-10-24):
- A0 done: plays well on the Pixel 9a; stuck notes and the slow interval found and fixed (decision log).
- A1 done: `tools/hil/ble_config_test.py` passes 19/19 from the Mac; the box id (board_id) is in the scan response,
  the mDNS TXT record and /api/status. Design note: `docs/design/config-over-ble.md`.
- A2 written: one list entry per box (BLE + mDNS merged by id), box screen over BLE or HTTP (status, mode, stored
  networks, Wi-Fi setup with the box's scan, phones, settings), pairing on first use, and the page follows the box
  when a mode change or a restart moves it to the other transport. The list merge and following are not yet tried
  on the phone (it was locked).

Code lives in `app/` in this repo (the API contract and both ends change together); design notes in `docs/design/`
per module as for the firmware. UI in the forslike style.

## Open questions

- Strip count and size on a phone screen: the iPad layout had 20 mm strips; the Pixel 9a is ~65 x 146 mm, so in
  portrait three strips need ~15 mm each with ~11 rows visible at 12 mm. A0 starts portrait, 15 / 3 / 11 mm.
- Answered 2026-10-24: phone = Pixel 9a on the latest Android; code in `app/` here; daisy_synth with an MPE mode
  plays the strips first; the pairing window is fine.

## Decision log

| Date | Decision | Why |
|---|---|---|
| 2026-10-24 | An Android app joins the web UI (supersedes "no app to install" for phones; the web UI stays) | setup over BLE without joining the AP; strips need native multi-touch and a direct BLE link |
| 2026-10-24 | Flutter (proposed, confirmed or reversed by A0) | touch, drawing and BLE writes in one Dart thread; mature BLE plugin; keeps the iPad possible |
| 2026-10-24 | Pixel 9a first; `app/` in this repo; daisy_synth MPE mode is the first synth; pairing window accepted | user's answers to the plan's questions |
| 2026-10-24 | A0 on the hardware: works, plays well; stuck notes came from the app resending the MPE zone setup (24 CCs) on every layout slider tick, which overflowed the box's TRS queue (3 207 dropped) | setup only when the strip count changes; 8 ms between batches; the box's merge now coalesces bends/pressure/CCs (docs/design/transports.md) |
| 2026-10-24 | The box asks for 7.5-15 ms first, Apple's 11.25-26.25 ms only if refused | the Pixel 9a took 26.25 ms from the Apple range |
| 2026-10-24 | Layout button: whole bar height, 112 px wide; key picked from 12 cells | user: the button was hard to hit, "key +" only stepped |
| 2026-10-24 | One API for HTTP and BLE (`components/api`), a GATT service carrying it (`ble_config`, framing in `lib/blecfg.c`), trusted phones + pairing window | design note config-over-ble.md; ESP-IDF's wifi_provisioning would bring a second BLE setup and cover Wi-Fi only |
| 2026-10-24 | Wi-Fi scan and try work in Play mode too (radio on for the probe only) | a phone on BLE never needs Setup mode |
| 2026-10-24 | The pairing window also opens for 2 min after a power-on | rev A has one button (8 s = Setup); power is physical access too |
| 2026-10-24 | An LL procedure collision is no refusal; the box re-asks for 7.5-15 ms when Android drifts to 48.75 ms after pairing | measured on the Pixel 9a: back to 15 ms |

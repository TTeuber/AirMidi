# Network MIDI 2.0 (`lib/nm2`, `lib/ump_endpoint`, `firmware/components/net_midi`)

The box is a Network MIDI 2.0 (UDP, M2-124) **host**: it announces itself over mDNS as `_midi2._udp` with its UMP
Endpoint Name and Product Instance Id, listens on UDP 5673, and accepts up to two clients (`PORT_NET_0/1` on the
bus). macOS 26 is a client: Audio MIDI Setup's MIDI Network window lists the box and connects; the session then
appears as a CoreMIDI port (spike S1).

## Layers

- `lib/nm2`: the session protocol as pure logic. Datagrams and the time go in; datagrams to send, UMPs to deliver
  and session changes come out through callbacks. Host-tested (32 tests, below).
- `lib/ump_endpoint`: answers the UMP Stream messages a client sends (Endpoint Discovery, Stream Configuration
  Request, Function Block Discovery) and builds the announcement the box sends when a session starts (below). One
  bidirectional function block for now; it declares MIDI-CI (message version 2), which passes through it to and
  from the device on TRS.
- `net_midi`: the socket, a task that receives with a 10 ms timeout (so `nm2_host_tick` runs at least every
  10 ms), the bridge to the bus, and per-session protocol translation: a client that asks for the MIDI 2.0
  protocol gets MIDI 1.0 sources upscaled (`m1_to_m2`); MIDI 2.0 from a client is scaled down by the MIDI 1.0
  outputs. One mutex covers `nm2` between the UDP task and the bus task; port up/down changes are applied after
  it is released (`bus_set_up` may wait for queue space while the bus task waits for the mutex).

## Getting MIDI through Wi-Fi in order

UDP loses, duplicates and reorders datagrams. The spike, which had none of this, lost 45 of 600 messages at 5 ms
spacing.

- **FEC (sending)**: every datagram repeats the two previous UMP Data commands, so one lost datagram costs
  nothing. After the data stops, zero-length commands follow at 20 and 40 ms (carrying the last real one twice
  more), then at 0.3, 1, 3 and 10 s, then nothing (M2-124 7.2.1).
- **Retransmit (sending)**: the last 32 commands are kept; a Retransmit Request is answered from them, or with a
  Retransmit Error when they are gone.
- **Order (receiving)**: duplicates are skipped by sequence number. A gap holds what arrives after it (up to 48
  commands) instead of delivering out of order, because late MIDI is better than reordered MIDI: a note off
  overtaking its note on leaves a note hanging. The host asks for a retransmit after 10 ms (every 20 ms after
  that) and gives up after 150 ms, delivering what it holds and counting the loss. 150 ms because the Wi-Fi link
  to the Mac stalls for up to ~100 ms (measured with ping); at 60 ms the box gave up on gaps that FEC would have
  filled a moment later.
- Sequence numbers start at 0 for every session (M2-124 7.1), so losses before the first datagram that arrives
  are a gap like any other.

The tests drive the host with a simulated channel that drops, duplicates and reorders datagrams (up to 70% loss):
what is delivered is always in order and never twice, everything up to the last delivered command is accounted
for as delivered or lost, and answering retransmit requests recovers most of what FEC alone cannot.

## Liveness, and surviving a restart

What macOS 26.5 does (found with a session-command log on the box and macOS's own session property):

- It pings a host every 60 s and answers the host's pings. (Phase 4 concluded the opposite: macOS never answered
  and sent nothing while idle. That was the test tool's fault: `nm2connect` added the host without the
  `authentication-data` entry Audio MIDI Setup writes, and macOS dropped such an entry ~5 s after connecting.)
- **It never invites a host again once a session has ended**, however it ended: a `Bye` from the host, or its own
  ping timeout (11 pings a second apart, then `Bye 0x04`). The host disappears from its list and the user has
  to reconnect in Audio MIDI Setup.

So a box that restarts (power cycle, update, crash, a mode switch) must not let the session end:

- After a restart the Mac still believes in its session; the spec's answer to its next ping or data, `Bye 0x05`
  (Session Not Established), makes it forget the box. Answering the ping as if nothing happened (phase 4) is no
  better: the Mac keeps sending into a session the box no longer has.
- Instead the box **takes the session back** (`lib/nm2`, "Resuming"): a client that pings or sends data without a
  session gets `Session Reset`, both sides number their commands from 0 on the reply, the box sends no data until
  then, and of the datagram that triggered it only the newest command is delivered (the FEC repeats in front may
  be minutes old). Five unanswered resets end the session quietly; an Invitation starts a fresh one. This
  deviates from M2-124 on purpose.
- The box keeps the last client that invited it in NVS and sends it `Session Reset` the moment it joins Wi-Fi,
  so MIDI from TRS reaches the Mac at once rather than after the Mac's next packet. A Mac that has forgotten the
  box (it rebooted, the user disconnected) answers `Bye` or nothing, and that is the end of it.
- The host still never drops a client for unanswered pings, now because ending a session loses macOS for good; a
  client silent for 60 s gives its slot to a new one when both are taken.

Measured (`tools/hil/resume_test.py`, 3 resets): the session is back 4.7-5.3 s after the reset (the Wi-Fi join
time), 20/20 messages each way, and macOS still lists the box.

## Telling the client who we are

macOS 26.5 never sends Endpoint Discovery or Function Block Discovery over a Network MIDI 2.0 session (every
datagram logged on the box: not one UMP Stream message). Its session port, "Network MIDI 2.0 Session 1", is a
CoreMIDI endpoint that exists whether or not a host is connected, and macOS seems to ask an endpoint only when it
appears. So through phase 5 macOS knew nothing about the box: sessions stayed in the MIDI 1.0 protocol, and macOS
ran no MIDI-CI over them (`MIDICIDeviceManager` found nothing, docs/design/midi-ci.md).

When a session starts, the box now sends unasked what a full Endpoint Discovery would get (`ump_endpoint_announce`:
Endpoint Info, Device Identity, name, Product Instance Id, Stream Configuration). M2-104 allows this: an endpoint
should send Endpoint Info when it changes (to the client, a new session is a new endpoint at the other end), and
may send the others at any time. macOS answers within ~100 ms:

1. Function Block Discovery (the first time only; it remembers the block after that);
2. a Stream Configuration Request for the MIDI 2.0 protocol, which the box grants;
3. MIDI-CI Discovery and PE Capabilities on the block, which reach the device on TRS.

The announcement waits until the client has sent something on the session (`heard` in `nm2`; macOS pings at once)
and, for a resumed session, until the Session Reset is answered. Sent 1 ms after Invitation Accepted, it made macOS
answer its own ping with `Bye 0x05` (Session Not Established) in 1 of 4 connects: the UMP Data apparently overtook
macOS's own setup of the session. With the wait: 8 connects out of 8.

Sessions with macOS now run in the MIDI 2.0 protocol, so MIDI 1.0 from TRS is translated up (M2-104). Eight
controllers no longer arrive as plain CCs: Bank Select (0, 32) travels with the next Program Change, and RPN/NRPN
selection (98-101) and Data Entry (6, 38) become one RPN/NRPN message. `latency.py` used to encode its sequence
numbers in every controller number and showed this as 32 of 300 lost; it now skips those eight.

## BLE and Wi-Fi share the radio

On the C6, Wi-Fi and BLE take turns on one radio. With the BLE stack running (even idle, advertising once a
second), network MIDI took 22-23 ms median; with BLE off, 6.7 ms Mac -> box and 4.2 ms box -> Mac, the spike's
numbers. So Network mode runs **without BLE** by default (setting `ble_in_network` keeps it on, at that cost), and
switching between modes that differ in BLE restarts the box (NimBLE does not come back cleanly after a deinit).
In Setup mode and with `ble_in_network`, BLE advertises once a second instead of every 100-150 ms. Wi-Fi power
save is off in Network mode: with modem sleep the router holds frames until the next beacon wake-up and MIDI
arrives in clumps ~100 ms apart.

## Tools

`tools/hil/nm2connect` connects macOS to the box without Audio MIDI Setup, by adding the box to the `peers` of
the `midi2.udp.session` property on CoreMIDI's "UMP Network" device, in the form Audio MIDI Setup writes
(`address`, `name`, `authentication-data`; macOS adds `state = running` while connected). Writing the property
rebuilds macOS's session and forgets live connections, and writing an unchanged list does nothing, so a fresh
invitation is `--remove` and then add. `tools/hil/latency.py "Network MIDI 2.0 Session"` measures through it.

Also seen while debugging: every rebuild of macOS's session sent an empty SysEx (F0 F7) out of the AudioFuse, which
the box forwarded. Harmless, and only caused by the tool.

## Measured (C6, phase 4 firmware, BLE off)

| Path | Spacing | Median | p95 | Max | Lost |
|---|---|---|---|---|---|
| Mac -> box -> TRS | 20 ms | 6.7 ms | 73-75 ms | 92-98 ms | 0/300 |
| TRS -> box -> Mac | 20 ms | 4.2 ms | 69-70 ms | 93 ms | 0/300 |
| Mac -> box -> TRS | 5 ms | 7.1 ms | 85 ms | 109 ms | 0/600 (36 gaps recovered) |
| TRS -> box -> Mac | 5 ms | 4.4 ms | 67 ms | 94 ms | 0/600 |

The tail is the Mac's own Wi-Fi: a ping from the Mac to the router alone had a maximum of 89 ms at the same time
(macOS's AWDL channel hopping is the usual cause).

Again after the announcement, in the MIDI 2.0 protocol (2026-10-08): 7.2 / 4.9 ms median at 20 ms spacing (0/300
lost each way), 7.6 / 5.4 ms at 5 ms (0/600). The translation costs nothing measurable.

## The web UI's control surface (`/ws`, `PORT_WEB`)

The play page (faders for common CCs, one octave of pads, channel and octave steppers, a short monitor) talks to
the box over a WebSocket: binary frames, each a run of UMP words, big-endian. Frames from a page go on the bus as
`PORT_WEB`; what the bus routes to `PORT_WEB` goes to every connected page (up to 4), one frame per bus batch, so
CCs coming in from a device move the faders. The async send keeps the frame header but not the payload, so each
frame gets its own copy, freed in the completion callback. In ESP-IDF 5.5 a WebSocket URI handler is not called for
the handshake; `ws_post_handshake_cb` registers the page (found when the page never became a destination).

This page is the fallback; phase 5 draws a device's own controls from its MIDI-CI control list.

Measured (`tools/hil/ws_test.py`, 100 CCs each way at 20 ms): page -> TRS 3.9 ms median, TRS -> page 10.1 ms.

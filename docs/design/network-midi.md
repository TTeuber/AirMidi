# Network MIDI 2.0 (`lib/nm2`, `lib/ump_endpoint`, `firmware/components/net_midi`)

The box is a Network MIDI 2.0 (UDP, M2-124) **host**: it announces itself over mDNS as `_midi2._udp` with its UMP
Endpoint Name and Product Instance Id, listens on UDP 5673, and accepts up to two clients (`PORT_NET_0/1` on the
bus). macOS 26 is a client: Audio MIDI Setup's MIDI Network window lists the box and connects; the session then
appears as a CoreMIDI port (spike S1).

## Layers

- `lib/nm2`: the session protocol as pure logic. Datagrams and the time go in; datagrams to send, UMPs to deliver
  and session changes come out through callbacks. Host-tested (26 tests, below).
- `lib/ump_endpoint`: answers the UMP Stream messages a client sends after connecting (Endpoint Discovery, Stream
  Configuration Request, Function Block Discovery). One bidirectional function block for now.
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

## Liveness: macOS does not answer pings

The first version dropped a client after three unanswered pings and lost every macOS session after 19 s:
macOS 26.5 never answers a host's Ping, and sends nothing at all while idle (found on the rig with a datagram
log). Now a client is pinged while silent (the spec asks for it) but never dropped for not answering; a client
silent for 60 s gives its slot to a new one when both are taken. A Bye from the client ends a session at once.

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
the `midi2.udp.session` property on CoreMIDI's "UMP Network" device (format read back from a session made by hand
in spike S1). `tools/hil/latency.py "Network MIDI 2.0 Session"` then measures through it.

## Measured (C6, phase 4 firmware, BLE off)

| Path | Spacing | Median | p95 | Max | Lost |
|---|---|---|---|---|---|
| Mac -> box -> TRS | 20 ms | 6.7 ms | 73-75 ms | 92-98 ms | 0/300 |
| TRS -> box -> Mac | 20 ms | 4.2 ms | 69-70 ms | 93 ms | 0/300 |
| Mac -> box -> TRS | 5 ms | 7.1 ms | 85 ms | 109 ms | 0/600 (36 gaps recovered) |
| TRS -> box -> Mac | 5 ms | 4.4 ms | 67 ms | 94 ms | 0/600 |

The tail is the Mac's own Wi-Fi: a ping from the Mac to the router alone had a maximum of 89 ms at the same time
(macOS's AWDL channel hopping is the usual cause).

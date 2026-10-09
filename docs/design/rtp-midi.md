# RTP-MIDI (`lib/rtpmidi`, `lib/rtp_journal`, `firmware/components/rtp_midi`)

The second network transport, beside [Network MIDI 2.0](network-midi.md): RTP-MIDI (RFC 6295) with Apple's session
protocol (AppleMIDI). Older macOS and iOS, and rtpMIDI on Windows, speak it; Network MIDI 2.0 is newer. The box
announces itself as `_apple-midi._udp` on UDP 5004 (control) and 5005 (data), takes up to two sessions
(`PORT_RTP_0/1`), and both run whenever the box is in Network mode.

## Layers

- `lib/rtpmidi`: sessions and packets as pure logic, like `lib/nm2`: datagrams and the time go in, datagrams to
  send, UMPs to deliver and session changes come out. Invitations (IN/OK/NO/BY) in both directions, clock sync
  (CK, three-way), receiver feedback (RS), the RTP header, the MIDI command section (delta times, running status,
  SysEx in segments), and the journal (below). 22 tests.
- `lib/rtp_journal`: the recovery journal, sending and receiving. 14 tests.
- `rtp_midi`: two sockets and a task (`select()` with a 10 ms timeout for the ticks), mDNS, the bridge to the bus.
  RTP-MIDI carries MIDI 1.0, so its ports are byte outputs to the bus: it merges their sources SysEx-aware, and MIDI
  2.0 from elsewhere is translated down on the way out.

## Lost packets: the recovery journal

RTP-MIDI does not retransmit. Every packet carries a journal: a summary of the stream since a checkpoint packet the
receiver has confirmed. A receiver that sees a gap in the sequence numbers compares the journal with what it has
delivered and plays the difference. A lost NoteOff is played late, so no note hangs, and a lost controller change
is sent with its last value. RFC 6295 calls this the "recovery journal mandate": no lasting damage from any loss.

What the box journals, per channel: Program Change with its bank (chapter P), Control Change (C), Pitch Bend (W),
notes on and off (N, with a "play it late" hint for NoteOns under 100 ms old) and Channel Pressure (T). All Notes
Off and Reset All Controllers are journalled by count (their value means nothing), and the notes they ended leave
the journal. Not journalled: RPN/NRPN (chapter M), poly pressure (A), note extras (E) and the system journal (SysEx,
clock, song position). Received journals are parsed whole; their unsupported chapters are skipped by length.

The checkpoint follows the receiver's feedback (RS, the "closed-loop" policy), so the journal only describes what
the receiver may not have seen. History is kept for 4 channels at once, not 16: each channel's state is ~800 bytes,
and 16 per session took 26 KB of the C6's RAM. A fifth busy channel takes the slot of the one idle longest; its
unconfirmed history is given up and counted. After the last MIDI, journal-only packets follow at 20, 100 and 500 ms
unless the receiver has confirmed everything, so a lost last packet is repaired without waiting for the next note.

The tests check the mandate as a property: random streams on several channels, both ways, through a channel that
loses 30-50% of packets and reorders some. At the end both sides hold the same notes, controllers, programs and
bends. A test that sends the same values twice cannot see a repair, though: a journal restores state, and a lost CC
whose value the receiver already has needs no repair. `latency.py` gives each run its own values for this reason.

## What macOS needed (found on the rig)

`tools/hil/rtp_test.py` runs against macOS 26's own RTP-MIDI driver. These all came out of it:

- **The M bit stays 0.** RFC 6295 says to set it in every packet that carries MIDI. With it set, macOS held each
  packet back ~25 ms and dropped some (86 of 300 at 20 ms spacing). Apple's own packets have it 0; with 0: 4-6 ms
  and nothing lost.
- **macOS drops MIDI for the first ~5 s of a session**, until its clock syncs have settled (six, 1.5 s apart). The
  test waits.
- **macOS closes a session nobody listens to.** With no CoreMIDI client holding the session's port open, it sends
  BY after four clock syncs. A DAW holds it open; the test keeps it open.
- **macOS timestamps matter forwards, not backwards.** It holds each packet until the packet's time on its own
  clock: stamped 10 s ahead, nothing arrived within the test; stamped 10 s behind, everything arrived at once.

## Surviving a restart

macOS keeps clock-syncing a box that restarted, gets no answer, sends BY after three tries (30 s), and does not
invite the box again (the same as with [Network MIDI 2.0](network-midi.md)). So the box takes the session back:

1. A clock sync or MIDI from a client without a session sets up a session from that datagram: the client's SSRC,
   its data port, and the control port one below. This only works if the box's own SSRC is the same as before, so
   it comes from the MAC address.
2. The box's session clock must not start again from 0. In 32-bit RTP time a restarted clock looks days ahead, and
   macOS would hold the box's MIDI until then (see the timestamps finding above). It comes from SNTP wall-clock time.
   Nothing is answered until SNTP has the time; after 20 s without it, the clock runs from boot (sessions still
   work, but a restarted box is not taken back).
3. The box's sequence numbers must carry on from before, or macOS takes every packet "behind" its last one for a
   late packet and drops it. The box sends two journal-only probes numbered half the range apart: exactly one is
   ahead of whatever macOS saw last. macOS's receiver feedback (RS) names the probe it took, and the box carries on
   after it. MIDI waits for that (1-2 s, at most 5) and is dropped meanwhile.

None of this is in AppleMIDI, which has no way back into a session. Measured: 5 of 5 resets taken back (6.5-13.4 s
after the reset: Wi-Fi, SNTP, and macOS's next clock sync), MIDI both ways after each.

## Connecting from the Mac

`MIDINetworkSession`, the public API, stays disabled when used from a command-line tool on macOS 26. Instead,
`tools/hil/rtpconnect` writes the peer into the `apple.midirtp.session` property of the session's entity on CoreMIDI's
"Network" device, in the form Audio MIDI Setup leaves there: `{name, address: "\t<service>\tlocal."}`. An address
with a port is dropped, and macOS adds `state = connected` itself. The session itself (here "RTP Session 3") is made
once in Audio MIDI Setup's MIDI Network window.

## Measured (C6, Network mode, BLE off)

| Path | Spacing | Median | p95 | Lost |
|---|---|---|---|---|
| Mac -> box -> TRS | 20 ms | 6.6-8.0 ms | 9.7-78 ms | 0/300 |
| TRS -> box -> Mac | 20 ms | 3.8-7.3 ms | 8.9-85 ms | 0/300 |
| Mac -> box -> TRS | 5 ms | 6.5-8.1 ms | 9.3-86 ms | 0/600 (repairs out of order) |
| TRS -> box -> Mac | 5 ms | 3.7-5.8 ms | 7.8-77 ms | 0/600 |

The ranges are across runs: on a quiet evening p95 was under 10 ms, on a busy one ~80 ms. The tail is the Mac's
Wi-Fi, as with Network MIDI 2.0. Towards the box, repaired commands arrive after the ones that overtook them; that
is the journal working, not a reordering bug.

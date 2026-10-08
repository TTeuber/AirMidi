# Core: UMP, MIDI 1.0 streams, translation (`lib/`)

The portable heart of the firmware. Plain C11, no ESP-IDF or FreeRTOS headers, so the same files build for the
host (unit tests, the Mac MIDI-CI stand-in later) and for the ESP32 (`firmware/components/airmidi_core` just
points at `lib/`).

## UMP as the internal format (`ump.h`)

Every message inside airmidi is a Universal MIDI Packet (M2-104). Each transport converts to UMP where it receives
and back where it sends; the router in between only ever sees UMP.

- `ump_t` is four 32-bit words; a message uses 1-4 of them depending on its Message Type (MT, top nibble of word
  0). `ump_words_for_mt()` covers the reserved MTs too, so a receiver can always skip what it does not understand.
- Builders for the types the bridge produces: MT 0x1 (system), 0x2 (MIDI 1.0 channel voice), 0x3 (SysEx7, up to
  6 bytes per packet), 0x4 (MIDI 2.0 channel voice). They mask data bytes to 7 bits, so a builder cannot produce
  an invalid packet from a bad byte.

**Alternatives.** A MIDI 1.0 byte router with UMP only at the network edge would be smaller today, but every
MIDI 2.0 feature (Network MIDI 2.0, function blocks, MIDI 2.0 values from the web UI) would then need a second
path. CoreMIDI and Windows MIDI Services made the same choice.

**Cost.** A 3-byte note becomes a 16-byte `ump_t` in queues. At MIDI rates (a few thousand messages per second at
most) that is a few KB of queue memory, which the C6's 512 KB SRAM affords.

## MIDI 1.0 bytes -> UMP (`midi1_in`)

A byte-at-a-time parser, one instance per byte source (TRS in, each BLE connection). It returns 0-2 UMPs per byte
instead of calling back, which keeps it re-entrant and trivial to test.

Rules it implements (MIDI 1.0 Detailed Specification), each with a test:

| Situation | Behaviour |
|---|---|
| Running status | a data byte after a complete channel message starts a new one with the same status |
| Real-time byte (F8-FF) anywhere, even between a status and its data or inside SysEx | emitted at once, nothing else changes |
| System common (F1-F6), F7, SysEx | cancel running status |
| A status byte during SysEx | ends the SysEx (packet with what was held), then is processed itself (F6 gives 2 UMPs) |
| Data byte with no status, undefined status (F4, F5, F9, FD), message cut short | dropped and counted in `dropped` |

SysEx is packed into 6-byte MT 0x3 packets. The parser holds up to 6 bytes until a 7th arrives (or F7), so it
knows whether a packet is the last one: no empty END packets, and a short SysEx is a single COMPLETE packet. The
price is that SysEx bytes leave one byte-time (0.32 ms at 31250 baud) later, and a real-time byte inside a SysEx
is emitted ahead of up to 6 SysEx bytes that arrived before it, which is allowed (real-time may interleave).

## UMP -> MIDI 1.0 bytes (`midi1_out`)

One encoder per byte output. Running status is optional per output (on for TRS, where it saves a third of the
bandwidth on note streams; BLE MIDI has its own rules). It is cancelled by system common and SysEx, untouched by
real-time, and `midi1_out_reset_running()` lets the transport force a status byte (e.g. after idle time, for
receivers that were plugged in mid-stream).

Defensive cases: a SysEx START whose END never came is closed with F7 before the next message, and a CONTINUE/END
without a START is dropped, so a lost packet upstream cannot leave a synth swallowing notes as SysEx data. MT 0x4
is translated down first (below); utility, stream, flex data and SysEx8 have no MIDI 1.0 form and produce nothing.

## MIDI 1.0 <-> MIDI 2.0 translation (`translate`)

The M2-104 Appendix D Default Translation, which a MIDI 2.0 device must support:

- **Scaling.** Up: Min-Center-Max (minimum, center and maximum map exactly; above the center the low bits are
  filled with a repeating pattern so 127 becomes 0xFFFFFFFF). Down: drop the low bits. Up-then-down is lossless,
  which the tests check for every 7- and 14-bit value, plus every 7-bit value against the spec's own 7->16 routine.
- **2.0 -> 1.0** (`m2_to_m1`, stateless): Note On velocity that would scale to 0 becomes 1 (0 would mean Note Off);
  RPN/NRPN become CC 101/100 (99/98), 6, 38; Program Change with the bank-valid flag becomes Bank Select MSB, LSB,
  Program Change; per-note messages and relative controllers have no 1.0 form and are dropped (D.2.8).
- **1.0 -> 2.0** (`m1_to_m2`, state per group and channel): Note On velocity 0 becomes Note Off with velocity
  0x8000; Bank Select is held and rides on the next Program Change; CC 6/38/98-101 are collected into one RPN/NRPN,
  sent when CC 38 arrives, or when a second CC 6 or a new parameter number shows the previous one is finished;
  the RPN null function (7F/7F) is not sent. Devices that never send CC 38 would leave the last Data Entry waiting,
  so `m1_to_m2_flush()` lets the caller time it out (D.3.3 suggests a short timeout).

Where the 1.0 -> 2.0 side is used: outputs that negotiated the MIDI 2.0 protocol (a Network MIDI 2.0 session can).
The router itself carries whatever came in; most sources here are MIDI 1.0 (TRS, BLE) and stay MT 0x2 end to end.

## Tests (`test/`)

`cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build`, built with
`-Wall -Wextra -Wpedantic -Werror` and AddressSanitizer + UndefinedBehaviorSanitizer.

- One test per rule in the tables above, with byte-exact expectations.
- A randomized round trip: 200 random MIDI 1.0 streams (running status, real-time bytes anywhere including inside
  SysEx, system common, SysEx of 0-40 bytes) through `midi1_in` and back through `midi1_out` must reproduce the
  input (real-time bytes compared by count, since they may move ahead of held SysEx bytes).
- A randomized 1.0 -> 2.0 -> 1.0 round trip over 20000 channel messages.
- A mutation pass when the tests were written: six deliberate bugs (scaling off by one, velocity-0 Note On allowed,
  running status kept after system common, SysEx packet sent one byte early, running status ignored by the
  encoder, bank MSB/LSB swapped). Five were caught; the sixth showed no test covered the value just above center,
  which the spec-reference test now does.

# The bridge: bus, TRS, BLE MIDI, button/LED, battery (phase 2)

```
 J3 IN ─ UART1 RX ─ trs_rx task ─ midi1_in ─┐                     ┌─ merge ─ midi1_out ─ UART1 TX ─ J2 OUT
                                            ├─▶ bus queue ─ bus ──┤
 BLE write ─ NimBLE host ─ blemidi_decode ──┘   (UMP)     task    └─ merge ─ blemidi_tx ─ notify (per central)
             ─ midi1_in (per central)            router_route()
```

## Bus (`firmware/components/bus`)

One FreeRTOS queue of `(port, UMP)` items and one task that owns everything downstream of it. Transports only
call `bus_send()` (non-blocking; a full queue drops and counts), so a slow output can never stall a radio stack's
task. The bus task:

1. takes everything waiting in the queue, routes each message (`router_route`: matrix, filters, remap), pushes it
   into each destination's merge (MIDI 1.0 wires) and drains that merge into the transport's `write()` straight
   away, as far as the transport has room (`room()`);
2. calls `flush()` once per batch, so a burst of messages becomes one BLE packet instead of one per message;
3. wakes at least every 10 ms for SysEx timeouts, outputs that had no room before, and the task watchdog.

**Backpressure, not drops.** A transport reports how much it can take (`room()`); while that is less than one
encoded message the bus leaves the rest in the output's merge queue, which is bounded and counts its overflow. The
transports themselves never block and only drop in a case the bus should prevent.

**Latest value wins.** A source can send faster than an output carries: the app's pedal steel strips send a pitch
bend per strip every few ms, TRS carries ~1000 messages/s. Waiting values are coalesced in the merge queue (pitch
bend, channel and poly pressure, ordinary controllers): a new one replaces the queued one for the same channel from
the same source, unless another message for that channel came between them (a note on in between would start at
the wrong bend). Bank select, RPN/NRPN and channel mode controllers only mean something in order and are never
merged. Before this (2026-10-24) a flood of 6 360 messages in 5 s overflowed the TRS queue, dropped 3 207
messages, note offs among them, and left notes hanging; now a bend flood never drops a note off (`test_merge.c`).
The console's port line shows `coalesced`.

Port up/down changes go through the same queue, so only the bus task touches the merges and a port coming up is
ordered with the traffic around it (an `on_up` hook resets the transport's send state there). Priority 15, above
the transports' own tasks: a burst arriving is routed before more piles up behind it.

**Alternatives.** Transports writing to each other directly (fewer copies) would mean every pair of transports
knows about every other and their locks; one queue and one consumer is easier to reason about and to test. A queue
per output would let outputs run in parallel, but every output here is non-blocking anyway.

## TRS (`uart_midi`)

UART1 at 31250 baud routed to D6/D7 (UART0's pins; the console is on USB). Receive: a task reads one byte, then
whatever is already buffered (spike S0: asking for a full buffer added the whole timeout to every message), with
the driver's RX timeout set to 1 byte-time so a message is handed over as soon as the line goes quiet. Send: running
status on (a third less wire time for note streams), forgotten after 1 s of silence so a device plugged in
meanwhile gets a status byte. The TX buffer is 8 KB (2.6 s of MIDI): BLE delivers a 1000-byte SysEx or a burst of
notes in a few connection events, far faster than 31250 baud drains it, and the first 1 KB buffer dropped those.

The TRS port is always up: there is no way to tell whether a cable is plugged into J2.

## BLE MIDI (`ble_midi`)

NimBLE peripheral with the BLE MIDI service, up to two centrals at once (`PORT_BLE_0/1`), still advertising while
a slot is free. Each slot has its own `midi1_in` (running status and SysEx continue across packets) and its own
`blemidi_tx` sized to the connection's MTU. A slot's port comes up when the central subscribes to notifications.

Two tasks share a slot: the NimBLE host task owns the connection, the parser and the counters; the bus task owns
the packetizer and the backlog. The only thing that crosses over, a new MTU, is handed through a field the bus task
applies before its next write.

Sending: when NimBLE has no buffer for a notification (a burst, or a 30 ms connection interval), the packet waits in
a 16-packet backlog and goes out on a later bus tick, in order. While the backlog is not empty the packet being built
keeps filling up to the MTU instead of being sent small, so a burst turns into a few full packets. Receiving: the
buffer takes any attribute size (512 bytes): the first version had 252 and silently dropped the 253-byte writes a
256-byte MTU allows.

Connection parameters: the box asks for 11.25-26.25 ms, the range Apple's accessory guidelines accept for MIDI
(min >= 11.25 ms, max >= min + 15 ms); macOS's MIDI driver then sets 7.5 ms on its own (spike S2). Received BLE
MIDI timestamps are ignored: messages go out as soon as they arrive. Scheduling by timestamp would remove the
connection-interval jitter (up to 7.5 ms) at the cost of a fixed delay of about the same size; for live playing
the lower latency wins. Measured numbers are in [measurements.md](../measurements.md).

## Button and LED (`ui_io`, `lib/src/ui.c`)

A 10 ms esp_timer samples the button and drives the LED. The gestures (debounced; short, double, long on release
at 2-8 s, very long fires at 8 s while still held) and the blink patterns are pure functions of (input, time) in
`lib/`, tested on the host including the millisecond clock wrapping. Double press = all notes off on every output
(sustain off, all sound off, all notes off on 16 channels).

## Battery (`power`, `lib/src/ui.c`)

Once a second: 16 calibrated ADC readings of A0 (half the switched cell), times two, plus a per-board correction
from a multimeter comparison (+14 mV on board #1). The filter is an 8-sample exponential average; "low" (LED warns)
below 3.5 V with 50 mV hysteresis; "empty" only after 5 consecutive filtered readings under 3.3 V, so a sag while the
radio transmits cannot shut the box down. Empty: notes off everywhere, then deep sleep with no wake source until
the power switch is cycled (a few µA).

A0 sits after the power switch, so with the switch off and USB plugged in it reads ~0 V. The first version took
that for an empty battery and put the board to sleep five seconds after boot; now anything under 2.5 V means "no
cell, on USB power" and is never low or empty (`test_no_cell_is_absent_not_empty` pins it).

## What the hardware test found

`tools/hil/bridge_test.py` (15 checks both ways through the AudioFuse) failed 6 of 15 on the first run, all real
bugs: the 252-byte receive buffer, notifications dropped instead of retried, the 1 KB UART buffer, and the bus
routing a whole queue before draining any output, so a 167-packet SysEx overflowed a 64-entry merge queue (and those
drops were not even counted). After the fixes above: 15/15, twice in a row, with 0 drops in every counter.

## Test hooks

Console commands on the USB port: `s` prints every counter (TRS bytes and drops, per-slot BLE state, interval,
MTU, packets, per-port in/out/dropped, battery, free heap); `m` injects 60 notes from the web UI's (otherwise unused)
port, so `tools/hil/bridge_test.py` can put two sources on the TRS output and check the merge on the real wire.

## Power modes

`power_set_mode()` picks one of three esp_pm configurations: fixed 160 MHz, frequency scaling 40-160 MHz, or
scaling plus automatic light sleep. The firmware boots with scaling. Light sleep is a test mode only: the HP UART
loses the bytes that wake it (confirmed on the rig with `tools/hil/sleep_test.py`, even with the UART on the
crystal clock), and the C6's USB serial/JTAG stops answering, reset included. Numbers and the reasoning are in
[measurements.md](../measurements.md#power).

# Phase 6: BLE central, USB MIDI, the ESP-NOW cable

## BLE central (`ble_midi`, PORT_BLE_CENTRAL)

The box connects by itself to one BLE MIDI device, a keyboard or controller, next to the two centrals that may
connect to it. The setting `ble_device` picks it: none, `*` (the first BLE MIDI device found), its advertised name,
or its address. The address option exists because macOS's own peripheral (the test stand-in) advertises no name at
all. A changed setting applies at once.

With a target set, the box scans a third of the time (30 ms in every 100, active, for names in scan responses)
until it sees the device, connects at 7.5-15 ms, exchanges the MTU, then finds the MIDI service, characteristic and
its CCCD and subscribes, one ATT request at a time. A device that wants pairing gets "just works" (no display, no
keys), bonded and kept in NVS, and the subscription is retried once encrypted. A device that asks for a slower
connection later is held to 15 ms: the central has the last word, and BLE MIDI wants 7.5-15 ms. Lost, the box
scans again. The web page's settings list the BLE MIDI devices around (`/api/ble`, a 5 s scan) to pick one.

Receiving from the device is the peripheral path in reverse (notifications in, the same parser); sending is a
write without response to the device's characteristic, with the same backlog as notifications.

Measured with `tools/hil/blepeer` (a CoreBluetooth peripheral on the Mac standing in for the keyboard),
`central_test.py` 14/14:

| Mode | device -> TRS | TRS -> device |
|---|---|---|
| Play (BLE alone) | 13.0 ms median, 18.6 p95 | 11.3 ms, 18.7 p95 |
| Network with BLE (radio shared with Wi-Fi) | 21.5 ms, 291 p95 | 16.4 ms, 46 p95 |

It reconnects 3-5 s after a setting change or a restart. The stand-in cannot play a device going away: quitting it
leaves macOS holding the link with the service gone, and CoreBluetooth cannot drop a central from the peripheral
side. macOS's peripheral also stops advertising after its first connection (a keyboard advertises whenever it is
not connected), so the test starts it afresh for each reconnection.

## USB MIDI (S3 only: `usb_midi`, PORT_USB, `lib/usbmidi`)

The S3 has USB OTG. TinyUSB (`espressif/esp_tinyusb`) makes it a composite device: a class-compliant USB MIDI 1.0
port named after the box, and a CDC serial port for the console. USB MIDI 2.0 is not offered: every host speaks
1.0, and MIDI-CI passes through it either way. `lib/usbmidi` turns UMP into USB-MIDI event packets (CIN per message
kind, SysEx 3 bytes a packet, MIDI 2.0 translated down as for TRS) and packets back into bytes for a parser; its
tests run every SysEx length through a clock-interrupted round trip.

TinyUSB takes the S3's only USB PHY from the built-in USB-Serial-JTAG, and that cost three findings on the dev kit:

- The console moves to the CDC port, and with it flashing: esptool's reset pattern on that port (RTS, then DTR) or
  console `B` restarts the chip into its ROM bootloader, which comes up on USB-Serial-JTAG under another name.
  `build.sh s3 flash` does that, flashes, and restarts the app with a watchdog reset. An RTS reset came back in
  download mode, because macOS holds DTR (= GPIO0 low) through it.
- The PHY choice survives a restart. Left on OTG, the next boot waited forever to write its log to a USB-Serial-JTAG
  nobody could reach, and the box vanished from USB. A shutdown handler now gives the PHY back before every
  restart, and the S3 no longer uses USB-Serial-JTAG as a console at all: a panic report hung there the same way,
  before the core dump was written. Before USB starts there is no console (the dev kit's UART port has one).
- TinyUSB's console drops output when nobody reads it. The C library then left stdout in its error state for good,
  so the box ran on but never printed again, which looked like a hang. The main loop clears it.

`usb_test.py` (BLE on the other side, so a bare dev kit is enough): 162/162 messages each way, in order, a
300-byte SysEx included; 7.8 / 8.7 ms median (BLE's interval is most of it).

## The ESP-NOW cable (`espnow_midi`, PORT_ESPNOW, `lib/espnow_link`)

Two or more boxes in the same group pass MIDI to each other straight from radio to radio, with no access point: a
wireless MIDI cable. Off by default (`espnow`, `espnow_group`); in Play mode it keeps the Wi-Fi radio on, without
joining anything.

Every box broadcasts a hello (group, name, next sequence number) once a second. A hello from the same group makes
the sender a peer; one silent for 5 s is gone. MIDI goes to each peer as UMP by unicast, which the radio
acknowledges and retries; the receiver drops the duplicates a lost acknowledgement causes, by sequence number, and
counts what never came. The port is UMP-native: each box translates at its own MIDI 1.0 outputs.

Both boxes need one channel. A box joined to an access point is on its channel and stays there. A free box looks:
back to channel 1 every other step, so two free boxes meet there, and in between to 6, 11 and the rest. Each box
moves at its own pace, so two cannot keep missing each other in step. A box in Play mode finds one in Network mode
by itself (within ~25 s); in the test, on channel 6, at once.

Found on the dev kit: at the full 20 dBm none of its Wi-Fi frames arrived anywhere (the C6 acknowledged no unicast,
and the access point never answered its authentication, "reason 2" on every join) while its BLE worked. At 10 and
15 dBm all did; most likely its 3.3 V sags at the transmit peaks on USB power. Boards now carry a Wi-Fi power limit
(the kit: 15 dBm), applied whenever Wi-Fi starts. At 15 dBm the kit joins Wi-Fi too.

`espnow_test.py` 9/9, the C6 in Network mode and the S3 in Play mode:

| Path | Spacing | Median | p95 | Lost |
|---|---|---|---|---|
| TRS -> C6 -> ESP-NOW -> S3 -> USB | 20 ms | 3.8 ms | 7.9 ms | 0/300 |
| USB -> S3 -> ESP-NOW -> C6 -> TRS | 20 ms | 4.1 ms | 8.1 ms | 0/300 |
| TRS -> C6 -> ESP-NOW -> S3 -> USB | 5 ms | 3.5 ms | 6.9 ms | 0/600 |
| USB -> S3 -> ESP-NOW -> C6 -> TRS | 5 ms | 4.0 ms | 8.5 ms | 0/600 |

The medians include the TRS wire (~1 ms a message) and USB. A 300-byte SysEx arrived whole each way.

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

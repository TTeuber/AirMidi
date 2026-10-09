# Measurements

Every number here was measured on the real hardware: receiver rev A with the XIAO ESP32-C6 (board #1), the Mac
(macOS 26.5) and an Arturia AudioFuse Studio cabled to the receiver's TRS jacks. Method first, then results;
each row says which firmware it was measured with.

## Method: one-way latency on one clock

```
 Mac ──BLE / Wi-Fi──▶ receiver ──TRS──▶ AudioFuse ──USB──▶ Mac        (wireless -> TRS)
 Mac ──USB──▶ AudioFuse ──TRS──▶ receiver ──BLE / Wi-Fi──▶ Mac        (TRS -> wireless)
```

Both ends are on the Mac, so `tools/hil/latency.py` timestamps the send and the receive with the same clock:
one-way latency without halving a round trip. Each message is a Control Change whose channel, controller and value
encode a sequence number, so loss, duplicates and reordering are counted too. 300 messages per run, 20 ms +-5 ms
apart (the jitter keeps the sends from locking to the BLE connection interval). The times include the AudioFuse's
own USB MIDI path and the TRS wire time (3 bytes at 31250 baud = 0.96 ms).

## Latency

| Path | Firmware | Median | p95 | Max | Lost |
|---|---|---|---|---|---|
| Mac -> AudioFuse -> receiver echo -> AudioFuse -> Mac (round trip, 3-byte messages) | spike S0 | 3.0 ms | | | 0/120 |
| BLE -> TRS, CoreMIDI BLE driver (7.5 ms interval), Wi-Fi off | spike S2 | 15.7 ms | 19.3 ms | 36.9 ms | 0/300 |
| TRS -> BLE, same | spike S2 | 9.1 ms | 12.5 ms | 32.6 ms | 0/300 |
| BLE -> TRS, Wi-Fi joined and idle | spike S2 | 16.2 ms | 24.8 ms | 39.1 ms | 0/300 |
| BLE -> TRS, Wi-Fi sending 700 KB/s UDP | spike S2 | 19.1 ms | 41.1 ms | 58.0 ms | 0/300 |
| BLE -> TRS, bleak instead of CoreMIDI (macOS keeps 30 ms) | spike S2 | 19.7 ms | 33.4 ms | 56.2 ms | 0/300 |
| Network MIDI 2.0 -> TRS | spike S1 | 6.4 ms | 71.8 ms | 95.5 ms | 0/300 |
| TRS -> Network MIDI 2.0 | spike S1 | 7.1 ms | 78.3 ms | 109.4 ms | 0/300 |
| BLE -> TRS, phase 2 bridge, bleak central, 7.5 ms interval | phase 2 | 8.4 ms | 12.0-12.7 ms | 29-31 ms | 0/300 |
| TRS -> BLE, same | phase 2 | 9.4-9.5 ms | 14.7-16.1 ms | 22-26 ms | 0/300 |
| Network MIDI 2.0 -> TRS, phase 4 (FEC, retransmit), BLE off | phase 4 | 6.7 ms | 73-75 ms | 98 ms | 0/300 |
| TRS -> Network MIDI 2.0, same | phase 4 | 4.2 ms | 69-70 ms | 94 ms | 0/300 |
| Network MIDI 2.0 -> TRS at 5 ms spacing | phase 4 | 7.1 ms | 85 ms | 109 ms | 0/600 |
| Network MIDI 2.0 -> TRS, BLE stack running | phase 4 | 23.0 ms | 102 ms | 156 ms | 0/300 |
| Web UI (WebSocket) -> TRS, 100 CCs | phase 4 | 3.9 ms | | 90 ms | 0/100 |
| TRS -> web UI (WebSocket) | phase 4 | 10.1 ms | | 97 ms | 0/100 |
| TRS -> web UI after the WebSocket send fix | phase 5 | 10.0 ms | | 97 ms | 0/100 |
| Network MIDI 2.0 -> TRS, MIDI 2.0 protocol (after the endpoint announcement) | phase 5 | 7.2 ms | | | 0/300 |
| TRS -> Network MIDI 2.0, same | phase 5 | 4.9 ms | | | 0/300 |
| RTP-MIDI (macOS driver) -> TRS, 20 ms spacing | phase 6 | 6.6-8.0 ms | 9.7-78 ms | | 0/300 |
| TRS -> RTP-MIDI, same | phase 6 | 3.8-7.3 ms | 8.9-85 ms | | 0/300 |
| RTP-MIDI -> TRS, 5 ms spacing (journal repairs) | phase 6 | 6.5-8.1 ms | 9.3-86 ms | | 0/600 |
| TRS -> RTP-MIDI, 5 ms spacing | phase 6 | 3.7-5.8 ms | 7.8-77 ms | | 0/600 |
| BLE device (box as central) -> TRS, Play mode | phase 6 | 13.0 ms | 18.6 ms | 19.4 ms | 0/100 |
| TRS -> BLE device, Play mode | phase 6 | 11.3 ms | 18.7 ms | 19.4 ms | 0/100 |
| BLE device -> TRS, Network mode with BLE | phase 6 | 21.5 ms | 291 ms | 434 ms | 0/100 |
| TRS -> BLE device, Network mode with BLE | phase 6 | 16.4 ms | 45.8 ms | 47.3 ms | 0/100 |
| BLE (bleak) -> S3 -> USB MIDI | phase 6 | 7.8-8.2 ms | | 19-29 ms | 0/162 |
| USB MIDI -> S3 -> BLE | phase 6 | 8.7-9.1 ms | | 27-28 ms | 0/162 |
| TRS -> C6 -> ESP-NOW -> S3 -> USB MIDI, 20 ms | phase 6 | 3.8 ms | 7.9 ms | | 0/300 |
| USB MIDI -> S3 -> ESP-NOW -> C6 -> TRS, 20 ms | phase 6 | 4.1 ms | 8.1 ms | | 0/300 |
| TRS -> ESP-NOW -> USB MIDI, 5 ms | phase 6 | 3.5 ms | 6.9 ms | | 0/600 |
| USB MIDI -> ESP-NOW -> TRS, 5 ms | phase 6 | 4.0 ms | 8.5 ms | | 0/600 |

The RTP-MIDI ranges are across runs on different evenings: the p95 follows the Mac's Wi-Fi (under 10 ms on a quiet
night, ~80 ms on a busy one). The ESP-NOW rows include the TRS wire (~1 ms a message) and USB at the S3 end; the
radio hop itself is about 2 ms. Updates over BLE: 1.6 MB in 69 s (23 KB/s, Play mode, bleak on macOS).

MIDI-CI over TRS (phase 5, `tools/hil/ci_test.py`): a full scan (Discovery, capabilities, 4 resources, 1.4 KB of
JSON) takes 720-910 ms, a rescan of a known device 37-74 ms, a scan with nothing on the jacks 3.0 s (the spec's
timeout).

The phase 2 rows come from two runs of `tools/hil/bridge_test.py --bleak`. They are lower than the spike's CoreMIDI
rows for BLE -> TRS: the bleak central writes as soon as it is asked, while CoreMIDI's driver schedules its sends.
The CoreMIDI path for the phase 2 firmware is still to be measured (its BLE driver refused to take the receiver
after spike S2's disconnect tests; see [design/testing.md](design/testing.md)).

The Network MIDI 2.0 tail is the Wi-Fi link itself: a plain `ping` to the receiver during the same session had a
maximum of 104 ms.

## Power

Phase 2 firmware, receiver on USB with the power switch off (the board runs from USB, the cell is out of the
circuit), a USB power meter in the receiver's cable. The meter is coarse: it reads in steps and shows 0.000 A for
currents under its threshold, so these are ranges read by eye, not averages. A shunt and an oscilloscope (or a
Nordic PPK2) would give real averages; until then these only rank the modes.

| State | Meter reading |
|---|---|
| A: CPU fixed 160 MHz, BLE advertising, not connected | 0 or 23-40 mA |
| B: CPU fixed 160 MHz, BLE connected (30 ms interval), idle | 0 or 23-50 mA |
| C: frequency scaling 40-160 MHz, connected, idle | 0 or 23-40 mA |
| D: frequency scaling + automatic light sleep, connected, idle | mostly 0, peaks 23-35 mA |

Light sleep is the only change the meter clearly shows, and it cannot be used while a TRS device may send: with
`tools/hil/sleep_test.py`, single messages after 1.5 s of silence arrived 15/15 both ways in modes 0 and 1, but in
light sleep only 9/15 TRS -> BLE (and some were lost inside a 20 ms stream), even with the UART clocked from the
crystal. BLE -> TRS survived light sleep (the radio wakes the chip). Light sleep also silences the C6's USB
serial/JTAG, including the reset the Mac uses, until the RESET button is pressed.

Decision: boot in mode C (frequency scaling: free, no loss). Light sleep only where no TRS input is lost: a future
"standby" (nothing connected for minutes) or rev B hardware with MIDI IN on the C6's LP UART pins (GPIO4/5, back
pads on the XIAO), which can receive while the main core sleeps.

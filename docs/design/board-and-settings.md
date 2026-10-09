# Board layer and settings

## Board layer (`firmware/components/board`)

The receiver rev A PCB takes either XIAO module on the same headers, so the signals are fixed and only the GPIO
numbers differ. `board.h` describes the signals (`board_t`: MIDI TX/RX, LED, button, battery ADC channel) and
`board()` returns the table for the board chosen in Kconfig (`CONFIG_AIRMIDI_BOARD_XIAO_C6` / `_XIAO_S3`, which
`sdkconfig.defaults.<target>` selects). Nothing else in the firmware names a GPIO.

| Signal | Header | C6 GPIO | S3 GPIO |
|---|---|---|---|
| MIDI TX -> J2 OUT | D6 | 16 | 43 |
| MIDI RX <- J3 IN | D7 | 17 | 44 |
| Battery sense (cell / 2) | A0 | 0 (ADC1 ch 0) | 1 (ADC1 ch 0) |
| Status LED | D1 | 1 | 2 |
| Button (to GND) | D2 | 2 | 3 |

`board_init()` does what has to happen before the radio starts. On the C6 module that is the RF switch (GPIO3 low
powers it) and the antenna select (GPIO14 low = ceramic antenna; the U.FL path measured ~20 dB weaker with nothing
connected).

**Why UART1 on UART0's pins.** D6/D7 are UART0's default pins. The console never goes there: on the C6 it is the
USB port (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`), on the S3 TinyUSB's serial port once USB starts
([transports.md](transports.md), USB MIDI). MIDI is driven by UART1 routed to those pins through the GPIO matrix, so
a log line can never reach the MIDI cable. One leak was left: the ROM bootloader prints its banner on UART0 TX right
after reset, before any of our code runs, which a synth reads as random Program Changes. Board #1's C6 has the
`UART_PRINT_CONTROL` eFuse burned against it (the plan's decision log); an S3 receiver needs the same.

**A third board: the ESP32-S3-DevKitC-1 on its own** (`build.sh s3kit`, `CONFIG_AIRMIDI_BOARD_DEVKITC_S3`), the
second box for USB MIDI and the ESP-NOW cable before a second receiver PCB exists. No LED (only an addressable RGB
LED on the kit), no battery (`batt_adc_channel = -1`: always "on USB"), BOOT (GPIO0) as the button, MIDI UART on
GPIO17/18 if jacks are wired (43/44 go to the kit's USB-UART bridge, which carries the boot log and panic reports).

**Wi-Fi transmit power** is a board property too (`wifi_max_tx`, in 0.25 dBm, 0 = the chip's 20 dBm), applied
whenever Wi-Fi starts. The dev kit is held to 15 dBm. At 20 dBm none of its Wi-Fi frames arrived anywhere, while its
BLE worked: most likely its 3.3 V sags at the transmit peaks on a USB port ([transports.md](transports.md), ESP-NOW).

**The S3 and RAM.** Without PSRAM the S3 had 7 KB of heap left in Play mode. PSRAM is used where the board has it
(8 MB octal on the XIAO S3 and on this kit; a board without it still boots), with Wi-Fi, lwIP and NimBLE buffers
there first. Network MIDI's session state (~45 KB) is allocated when the box first enters Network mode, and BLE send
backlogs when a connection first needs one. The default event task's stack went from 2.3 to 4 KB: the first S3 to
join Wi-Fi overflowed it (Xtensa frames are bigger).

**Alternatives.** A devicetree-like runtime table or one firmware for both chips: not possible, the C6 is RISC-V
and the S3 Xtensa, so there are two images anyway and a compile-time table is the simplest correct thing.

## Settings (`lib/src/settings.c` + `firmware/components/nvs_settings`)

The rules live in the portable core and are host-tested: the fields (`settings_t`: name, mode, SysEx timeout, BLE
in Network mode, the BLE device to connect to, the ESP-NOW cable and its group), the defaults (the name ends in the
last two MAC bytes, `airmidi-7530`, so two boxes in one room differ), and `settings_check()`, which returns a reason
string the web UI will show. The name doubles as mDNS host name, so it
follows RFC 1123 label rules (letters, digits, '-', not at either end, at most 31 characters here).

Storage is NVS, namespace `airmidi`:

- **One key per field plus a `schema` number**, not one blob. After an OTA rollback the older firmware still reads
  the fields it knows and ignores newer keys; a blob would have to be versioned and converted both ways.
- **Migration** runs at boot when the stored schema is older (`migrate()`, a fall-through switch, one step per
  version). Schema 2 added `ble_in_network`, 3 `ble_device`, 4 `espnow` and `espnow_group`. A newer schema (after a
  rollback) is logged and read as far as this firmware understands it.
- Missing keys keep their defaults; if the stored values fail `settings_check()`, the defaults are used for this
  boot without overwriting the store.
- `nvs_settings_get()` hands out a copy under a mutex, so any task can read the settings without races.

Tested: the rules on the host (`test/test_settings.c`); the store on the board (first boot writes defaults, the
next boot reads them back).

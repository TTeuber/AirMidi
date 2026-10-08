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

**Why UART1 on UART0's pins.** D6/D7 are UART0's default pins. The console goes to the USB port on both boards
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`), and MIDI is driven by UART1 routed to those pins through the GPIO matrix,
so a log line can never reach the MIDI cable. One leak remains: the ROM bootloader prints its banner on UART0 TX at
115200 baud right after reset, before any of our code runs. At 31250 baud that arrives as a few framing errors on
the connected device. Silencing it means burning an eFuse (`UART_PRINT_CONTROL`), which is irreversible on the only
C6 board, so it is left alone and documented.

**Alternatives.** A devicetree-like runtime table or one firmware for both chips: not possible, the C6 is RISC-V
and the S3 Xtensa, so there are two images anyway and a compile-time table is the simplest correct thing.

## Settings (`lib/src/settings.c` + `firmware/components/nvs_settings`)

The rules live in the portable core and are host-tested: the fields (`settings_t`: name, mode, SysEx timeout),
the defaults (the name ends in the last two MAC bytes, `airmidi-7530`, so two boxes in one room differ), and
`settings_check()`, which returns a reason string the web UI will show. The name doubles as mDNS host name, so it
follows RFC 1123 label rules (letters, digits, '-', not at either end, at most 31 characters here).

Storage is NVS, namespace `airmidi`:

- **One key per field plus a `schema` number**, not one blob. After an OTA rollback the older firmware still reads
  the fields it knows and ignores newer keys; a blob would have to be versioned and converted both ways.
- **Migration** runs at boot when the stored schema is older (`migrate()`, a fall-through switch, one step per
  version). A newer schema (after a rollback) is logged and read as far as this firmware understands it.
- Missing keys keep their defaults; if the stored values fail `settings_check()`, the defaults are used for this
  boot without overwriting the store.
- `nvs_settings_get()` hands out a copy under a mutex, so any task can read the settings without races.

Tested: the rules on the host (`test/test_settings.c`); the store on the board (first boot writes defaults, the
next boot reads them back).

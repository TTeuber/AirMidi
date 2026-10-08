// Battery: the cell voltage through the 1:2 divider on A0, sampled once a second, filtered by lib/ui's battery
// logic (low warning with hysteresis, "empty" only when sustained).
#pragma once

#include <stdbool.h>
#include <stdint.h>

void power_start(void);

uint32_t power_cell_mv(void);  // filtered, 0 before the first sample
int power_percent(void);
bool power_low(void);
bool power_on_usb(void);  // no cell voltage: the switch is off and USB powers the board
bool power_empty(void);

// Power management modes (being measured, phase 2):
//   0 = CPU fixed at 160 MHz, no light sleep
//   1 = frequency scaling 160 / 40 MHz, no light sleep
//   2 = frequency scaling + automatic light sleep. Test only: MIDI arriving on TRS while asleep is lost (the byte
//       that wakes the UART, and more), and the USB serial/JTAG stops answering until the RESET button.
// The firmware boots in mode 1.
void power_set_mode(int mode);
int power_mode(void);

// Sleep until the power switch is cycled: no wake source, a few µA. Call after silencing the outputs.
void power_shutdown(void) __attribute__((noreturn));

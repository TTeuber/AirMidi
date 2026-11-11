// Battery: the cell voltage through the 1:2 divider on A0, sampled once a second, filtered by lib/ui's battery
// logic (low warning with hysteresis, "empty" only when sustained).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hal/gpio_types.h"

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

// Sleep until the power switch is cycled or, on a panel board, PWR is pressed (a few µA).
// Call after silencing the outputs.
void power_shutdown(void) __attribute__((noreturn));

// Panel boards (rev B): the PWR button is the power switch's soft twin. power_off() sleeps until it is pressed;
// power_confirm_wake(), early at boot, sends a box that button woke back to sleep unless the button is still down
// POWER_ON_HOLD_MS after the press (a knock in a bag does not start it). Call after silencing the outputs.
#define POWER_ON_HOLD_MS 1000
void power_off(gpio_num_t button) __attribute__((noreturn));
void power_confirm_wake(gpio_num_t button);

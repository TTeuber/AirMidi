#!/bin/sh
# Build the firmware; `./build.sh flash` also programs it over the J-Link (SWD) and resets.
# LIBDAISY_DIR defaults to ~/Developer/libDaisy (libDaisy v9, built once with `make` there).
set -e
cd "$(dirname "$0")"
make -j8 2>&1 | grep -vE "^(arm-none-eabi-|mkdir)" || true
test -f build/daisy_synth.elf
arm-none-eabi-size build/daisy_synth.elf
if [ "$1" = "flash" ]; then
    openocd -f flash.cfg -c "program build/daisy_synth.elf verify reset exit"
fi

#!/bin/bash
# Build (and optionally flash) the firmware for one board, each in its own build directory:
#   ./build.sh [c6|s3|s3kit] [flash] [monitor]       default c6; PORT=/dev/cu.usbmodemXXXX to pick the port
#   (s3kit: a bare ESP32-S3-DevKitC-1 instead of the receiver PCB)
set -e
cd "$(dirname "$0")"
board=${1:-c6}
case $board in
    c6) target=esp32c6 ;;
    s3) target=esp32s3 ;;
    s3kit) target=esp32s3 board_defaults=sdkconfig.devkitc_s3 ;;
    *) echo "usage: $0 [c6|s3|s3kit] [flash] [monitor]"; exit 1 ;;
esac
shift || true
export IDF_PATH=${IDF_PATH:-$HOME/Developer/esp-idf}
source "$IDF_PATH/export.sh" >/dev/null
dir=${BUILD_DIR:-build_$board}
# Images are signed at build time; a dev key is made on first use (gitignored; releases use the CI secret).
if [ ! -f keys/dev_signing_key.pem ]; then
    mkdir -p keys
    espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/dev_signing_key.pem
fi
# ESP-IDF only reads sdkconfig.defaults* when it creates sdkconfig; start over when they are newer than it.
if [ -f "$dir/sdkconfig" ] && [ -n "$(find sdkconfig.defaults* ${board_defaults:-} -newer "$dir/sdkconfig")" ]; then
    echo "sdkconfig.defaults changed: regenerating $dir/sdkconfig"
    rm "$dir/sdkconfig"
fi
# SDKCONFIG per board too, so switching boards never mixes configurations.
idf=(idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D IDF_TARGET=$target)
# Test images (tools/hil/ota_test.py): an extra defaults file and a version label.
extra="${board_defaults:-}${board_defaults:+${EXTRA_DEFAULTS:+;}}${EXTRA_DEFAULTS:-}"
if [ -n "$extra" ]; then idf+=(-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$target;$extra"); fi
if [ -n "$PROJECT_VER" ]; then idf+=(-D PROJECT_VER="$PROJECT_VER"); fi
"${idf[@]}" build
if [ "$1" = flash ]; then
    port=${PORT:-}
    if [ -z "$port" ] && [ $target = esp32s3 ]; then
        # The S3 app runs TinyUSB: its serial port is the app's (named after its MAC address, 12 hex digits), not
        # the chip's USB-Serial-JTAG. esptool's reset pattern on it restarts the chip into the ROM bootloader
        # (components/usb_midi), which comes back as USB-Serial-JTAG under another name.
        app=$(ls /dev/cu.usbmodem* 2>/dev/null | grep -E 'usbmodem[0-9A-F]{12}' | head -1)
        if [ -n "$app" ]; then
            before=$(ls /dev/cu.usbmodem*)
            python -c "
import serial, sys, time
s = serial.Serial('$app'); s.dtr = False; s.rts = True; time.sleep(0.1); s.dtr = True; s.rts = False; time.sleep(0.1)
try: s.close()
except Exception: pass"
            for _ in $(seq 1 50); do
                port=$(comm -13 <(echo "$before") <(ls /dev/cu.usbmodem* 2>/dev/null) | head -1)
                [ -n "$port" ] && break
                sleep 0.1
            done
            echo "$app -> bootloader on ${port:-?}"
        fi
    fi
    port=${port:-$(ls /dev/cu.usbmodem* | head -1)}
    "${idf[@]}" -p "$port" flash
    # On the S3's USB-Serial-JTAG, macOS holds DTR (= GPIO0 low) through esptool's RTS reset, so the chip comes back
    # in download mode; a watchdog reset starts the app.
    if [ $target = esp32s3 ]; then esptool.py --chip esp32s3 -p "$port" --before no_reset --after watchdog_reset read_mac >/dev/null; fi
    shift
fi
[ "$1" = monitor ] && "${idf[@]}" -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" monitor
exit 0

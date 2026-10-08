#!/bin/bash
# Build (and optionally flash) the firmware for one board, each in its own build directory:
#   ./build.sh [c6|s3] [flash] [monitor]       default c6; PORT=/dev/cu.usbmodemXXXX to pick the port
set -e
cd "$(dirname "$0")"
board=${1:-c6}
case $board in
    c6) target=esp32c6 ;;
    s3) target=esp32s3 ;;
    *) echo "usage: $0 [c6|s3] [flash] [monitor]"; exit 1 ;;
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
if [ -f "$dir/sdkconfig" ] && [ -n "$(find sdkconfig.defaults* -newer "$dir/sdkconfig")" ]; then
    echo "sdkconfig.defaults changed: regenerating $dir/sdkconfig"
    rm "$dir/sdkconfig"
fi
# SDKCONFIG per board too, so switching boards never mixes configurations.
idf=(idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D IDF_TARGET=$target)
# Test images (tools/hil/ota_test.py): an extra defaults file and a version label.
if [ -n "$EXTRA_DEFAULTS" ]; then idf+=(-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$target;$EXTRA_DEFAULTS"); fi
if [ -n "$PROJECT_VER" ]; then idf+=(-D PROJECT_VER="$PROJECT_VER"); fi
"${idf[@]}" build
if [ "$1" = flash ]; then
    "${idf[@]}" -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" flash
    shift
fi
[ "$1" = monitor ] && "${idf[@]}" -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" monitor
exit 0

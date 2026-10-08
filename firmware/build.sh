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
dir=build_$board
# SDKCONFIG per board too, so switching boards never mixes configurations.
idf=(idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D IDF_TARGET=$target)
"${idf[@]}" build
if [ "$1" = flash ]; then
    "${idf[@]}" -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" flash
    shift
fi
[ "$1" = monitor ] && "${idf[@]}" -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" monitor
exit 0

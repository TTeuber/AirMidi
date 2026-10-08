#!/bin/bash
# Spike S3: MIDI-CI initiator over TRS on the receiver (XIAO ESP32C6). ./build.sh [flash]
set -e
cd "$(dirname "$0")"
export IDF_PATH=${IDF_PATH:-$HOME/Developer/esp-idf}
source "$IDF_PATH/export.sh" >/dev/null
idf.py build
[ "$1" = flash ] && idf.py -p "${PORT:-$(ls /dev/cu.usbmodem* | head -1)}" flash
exit 0

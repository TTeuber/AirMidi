#!/bin/bash
# Build the Mac MIDI-CI device stand-in from lib/'s sources: ./build.sh  ->  build/mac_standin
set -e
cd "$(dirname "$0")"
mkdir -p build
lib=../../lib
clang -std=c11 -O2 -Wall -Wextra -Werror -I$lib/include -o build/mac_standin main.c \
    $lib/src/ump.c $lib/src/json_scan.c $lib/src/midici.c $lib/src/midici_responder.c \
    -framework CoreMIDI -framework CoreFoundation

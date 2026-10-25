#!/bin/bash
# Build and run the desktop check (test.cpp): ./host/build.sh  ->  host/build/test, host/build/demo.wav, host/build/ci/
set -e
cd "$(dirname "$0")"
mkdir -p build
lib=../../../lib
clang -std=c11 -O2 -Wall -Wextra -I$lib/include -c $lib/src/json_scan.c $lib/src/midici.c \
    $lib/src/midici_responder.c $lib/src/midici_initiator.c
mv ./*.o build/
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-format-security -I$lib/include -o build/test test.cpp \
    ../engine/controls.cpp ../engine/synth.cpp ../engine/fx.cpp ../engine/device.cpp build/*.o
cd build/.. && build/test

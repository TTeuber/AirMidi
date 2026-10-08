# Portable core: plain C11, no ESP-IDF or FreeRTOS headers. Built for the host by test/ and for the ESP32 by
# firmware/components/airmidi_core, from the same sources.
add_library(airmidi_core STATIC
    src/ump.c
    src/midi1_in.c
    src/midi1_out.c
    src/translate.c
)
target_include_directories(airmidi_core PUBLIC include)

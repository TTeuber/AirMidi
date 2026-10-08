# Portable core: plain C11, no ESP-IDF or FreeRTOS headers. Built for the host by test/ and for the ESP32 by
# firmware/components/airmidi_core, from the same sources.
add_library(airmidi_core STATIC
    src/ump.c
    src/midi1_in.c
    src/midi1_out.c
    src/translate.c
    src/settings.c
    src/blemidi.c
    src/router.c
    src/merge.c
    src/ui.c
    src/nm2.c
    src/ump_endpoint.c
    src/json_scan.c
    src/midici.c
    src/midici_initiator.c
    src/midici_responder.c
    src/ci_guard.c
    src/rtp_journal.c
    src/rtpmidi.c
)
target_include_directories(airmidi_core PUBLIC include)

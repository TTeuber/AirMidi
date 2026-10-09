idf_component_register(SRCS "ble_midi.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core bt
                       PRIV_REQUIRES bus esp_timer)

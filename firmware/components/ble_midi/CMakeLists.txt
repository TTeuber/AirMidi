idf_component_register(SRCS "ble_midi.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES bus airmidi_core bt esp_timer)

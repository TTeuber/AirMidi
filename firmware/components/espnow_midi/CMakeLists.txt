idf_component_register(SRCS "espnow_midi.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core
                       PRIV_REQUIRES bus net esp_wifi esp_timer)

idf_component_register(SRCS "rtp_midi.c" INCLUDE_DIRS "include" REQUIRES airmidi_core
                       PRIV_REQUIRES bus net lwip esp_timer esp_hw_support mdns nvs_flash)

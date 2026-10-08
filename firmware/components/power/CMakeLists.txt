idf_component_register(SRCS "power.c" INCLUDE_DIRS "include" REQUIRES airmidi_core
                       PRIV_REQUIRES board esp_adc esp_system)

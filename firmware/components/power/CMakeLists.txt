idf_component_register(SRCS "power.c" INCLUDE_DIRS "include" REQUIRES airmidi_core hal
                       PRIV_REQUIRES board esp_adc esp_system esp_pm esp_driver_gpio esp_timer)

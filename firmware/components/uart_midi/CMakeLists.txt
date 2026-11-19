idf_component_register(SRCS "uart_midi.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core
                       PRIV_REQUIRES bus board esp_driver_uart esp_driver_gpio esp_timer esp_system)

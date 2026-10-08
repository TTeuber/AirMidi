idf_component_register(SRCS "uart_midi.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES bus board airmidi_core esp_driver_uart esp_timer esp_system)

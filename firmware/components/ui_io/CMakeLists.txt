idf_component_register(SRCS "ui_io.c" INCLUDE_DIRS "include" REQUIRES airmidi_core
                       PRIV_REQUIRES board bus esp_timer esp_driver_gpio)

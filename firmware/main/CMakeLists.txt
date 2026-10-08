idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES board nvs_settings airmidi_core esp_app_format esp_system esp_driver_gpio)

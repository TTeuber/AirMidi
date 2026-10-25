idf_component_register(SRCS "ble_config.c" INCLUDE_DIRS "include"
                       REQUIRES bt
                       PRIV_REQUIRES airmidi_core nvs_flash esp_timer)

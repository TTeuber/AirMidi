idf_component_register(SRCS "ble_update.c" INCLUDE_DIRS "include"
                       REQUIRES bt
                       PRIV_REQUIRES ota app_update esp_app_format esp_system)

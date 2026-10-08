idf_component_register(SRCS "ota.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update espcoredump esp_timer esp_partition esp_system spi_flash esp_http_client
                                     mbedtls json esp_app_format)

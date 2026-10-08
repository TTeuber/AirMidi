idf_component_register(SRCS "nvs_settings.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core PRIV_REQUIRES nvs_flash esp_hw_support)

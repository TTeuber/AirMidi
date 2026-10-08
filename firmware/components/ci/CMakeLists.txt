idf_component_register(SRCS "ci.c" INCLUDE_DIRS "include" REQUIRES esp_common
                       PRIV_REQUIRES airmidi_core bus json esp_app_format esp_timer esp_system vfs)

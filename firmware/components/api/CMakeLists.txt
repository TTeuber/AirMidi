idf_component_register(SRCS "api.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core
                       PRIV_REQUIRES json esp_app_format esp_system esp_timer app_update nvs_settings net ota bus
                                     uart_midi ble_midi ble_config power board net_midi rtp_midi espnow_midi ci)

idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES board nvs_settings airmidi_core bus uart_midi ble_midi ui_io power ota net net_midi rtp_midi usb_midi espnow_midi webui api ble_config ci
                                     esp_app_format esp_system esp_driver_gpio esp_timer)

idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_driver_uart esp_driver_gpio
                                     esp_timer lwip)
# Dev Wi-Fi credentials from the gitignored .wifi at the repo root (line 1 SSID, line 2 password; copy
# .wifi.example). Left at the placeholder, the firmware builds without Wi-Fi.
set(wifi_file "${CMAKE_CURRENT_LIST_DIR}/../../../.wifi")
if(EXISTS ${wifi_file})
    file(STRINGS ${wifi_file} wifi_lines LIMIT_COUNT 2)
    list(GET wifi_lines 0 ssid)
    list(GET wifi_lines 1 pass)
endif()
if(DEFINED ssid AND NOT ssid STREQUAL "your-ssid")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_SSID="${ssid}" WIFI_PASS="${pass}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${wifi_file})

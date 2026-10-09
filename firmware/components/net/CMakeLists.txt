idf_component_register(SRCS "net.c" INCLUDE_DIRS "include" REQUIRES airmidi_core
                       PRIV_REQUIRES board esp_wifi esp_netif esp_event esp_timer nvs_flash mdns)
# Dev builds: seed the network list from the gitignored .wifi at the repo root (line 1 SSID, line 2 password;
# copy .wifi.example). Left at the placeholder, or absent (CI, releases), nothing is seeded.
set(wifi_file "${CMAKE_CURRENT_LIST_DIR}/../../../.wifi")
if(EXISTS ${wifi_file})
    file(STRINGS ${wifi_file} wifi_lines LIMIT_COUNT 2)
    list(LENGTH wifi_lines n)
    if(n EQUAL 2)
        list(GET wifi_lines 0 ssid)
        list(GET wifi_lines 1 pass)
        if(NOT ssid STREQUAL "your-ssid")
            target_compile_definitions(${COMPONENT_LIB} PRIVATE DEV_WIFI_SSID="${ssid}" DEV_WIFI_PASS="${pass}")
        endif()
    endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${wifi_file})

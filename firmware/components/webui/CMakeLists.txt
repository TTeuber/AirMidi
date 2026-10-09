idf_component_register(SRCS "webui.c" INCLUDE_DIRS "include"
                       REQUIRES airmidi_core
                       PRIV_REQUIRES esp_http_server json esp_app_format esp_system esp_timer app_update nvs_settings net ota bus
                                     uart_midi ble_midi power board net_midi rtp_midi ci)
# The web UI (web/ at the repo root) is gzipped at build time and linked into the image, so the UI always
# matches the firmware it came with.
set(web_dir ${CMAKE_CURRENT_LIST_DIR}/../../../web)
foreach(f index.html app.js style.css geist-medium.woff2)
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${f}.gz)
    add_custom_command(OUTPUT ${gz}
                       COMMAND ${CMAKE_COMMAND} -E copy ${web_dir}/${f} ${CMAKE_CURRENT_BINARY_DIR}/${f}
                       COMMAND gzip -9 -n -f ${CMAKE_CURRENT_BINARY_DIR}/${f}
                       DEPENDS ${web_dir}/${f}
                       COMMENT "gzip web/${f}")
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY DEPENDS ${gz})
endforeach()

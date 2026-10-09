# USB MIDI needs USB OTG: the S3 has it, the C6 does not (it gets functions that do nothing).
if(IDF_TARGET STREQUAL "esp32s3")
    idf_component_register(SRCS "usb_midi.c" INCLUDE_DIRS "include"
                           REQUIRES airmidi_core
                           PRIV_REQUIRES bus esp_system esp_hw_support esp_timer espressif__esp_tinyusb)
else()
    idf_component_register(SRCS "usb_midi_none.c" INCLUDE_DIRS "include")
endif()

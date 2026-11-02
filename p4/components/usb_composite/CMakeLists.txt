idf_component_register(SRCS "usb_composite.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "tusb"
                       PRIV_REQUIRES esp_hw_support esp_timer esp_mm)

# TinyUSB picks up this component's tusb_config.h; the descriptor callbacks
# are compiled into the TinyUSB library itself (no link-order dependency).
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb")
target_sources(${tusb_lib} PRIVATE "${COMPONENT_DIR}/tusb/usb_descriptors.c")
target_link_libraries(${COMPONENT_LIB} PUBLIC ${tusb_lib})

idf_component_register(SRCS "gnss_bridge.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES fpv_board usb_composite esp_driver_uart esp_driver_gpio esp_timer)

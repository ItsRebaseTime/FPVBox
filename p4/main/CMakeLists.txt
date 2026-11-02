idf_component_register(SRCS "app_main.c" "control.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fpv_board video_pipe usb_composite gnss_bridge c5_link elrs_tx
                                     nvs_flash esp_timer)

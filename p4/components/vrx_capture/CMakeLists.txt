idf_component_register(SRCS "vrx_capture.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_parlio esp_driver_gpio
                       PRIV_REQUIRES fpv_board esp_mm heap)

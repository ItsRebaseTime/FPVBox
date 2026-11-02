idf_component_register(SRCS "video_pipe.c" "osd.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES cvbs_decode usb_composite
                       PRIV_REQUIRES vrx_capture esp_driver_jpeg esp_mm esp_timer espressif__esp_h264)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

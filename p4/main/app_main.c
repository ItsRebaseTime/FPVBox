/* FPVBox ESP32-P4 firmware: analog 5.8 GHz video (C5VRX on the ESP32-C5) to
 * UVC, ELRS TX over CRSF, GNSS NMEA; one USB high-speed composite device. */
#include <stdio.h>
#include "c5_link.h"
#include "control.h"
#include "elrs_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_bridge.h"
#include "nvs_flash.h"
#include "usb_composite.h"
#include "video_pipe.h"

static const char *TAG = "fpvbox";

static void on_c5_channel(const char *name, uint16_t mhz, void *ctx)
{
    (void)ctx;
    char l1[40];
    snprintf(l1, sizeof(l1), "%s  %u MHZ", name, mhz);
    video_pipe_set_info(l1, "");
}

static void log_line(const char *line, void *ctx)
{
    (void)ctx;
    printf("%s", line);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The pipeline first: USB callbacks post into its queues. */
    ESP_ERROR_CHECK(video_pipe_start());
    ESP_ERROR_CHECK(usb_composite_start(video_pipe_usb_callbacks()));
    ESP_ERROR_CHECK(c5_link_start(control_c5_output, on_c5_channel, NULL));
    control_start();
    ESP_ERROR_CHECK(gnss_bridge_start());
    err = elrs_tx_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "ELRS TX not started: %s", esp_err_to_name(err));
    video_pipe_set_info("WAITING FOR C5VRX", "");
    ESP_LOGI(TAG, "running");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        control_print_status(log_line, NULL);
    }
}

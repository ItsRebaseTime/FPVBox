/* ESP32-C5 (C5VRX) control link (see c5_link.h). */
#include "c5_link.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fpv_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c5";
static c5_output_cb_t s_out;
static c5_channel_cb_t s_chan;
static void *s_ctx;
static c5_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_line[160];
static size_t s_line_len;

void c5_link_reset(void)
{
    /* CHIP_PU low >= 50 us; keep it generous for the module's RC. */
    gpio_set_level(FPV_C5_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(FPV_C5_EN_GPIO, 1);  /* open drain: released */
    portENTER_CRITICAL(&s_lock);
    s_st.resets++;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "C5 reset");
}

/* "[CHANNEL] R1 5658 MHz ..." (C5VRX control.c). */
static void parse_line(const char *l)
{
    char name[12];
    unsigned mhz;
    if (sscanf(l, "[CHANNEL] %11s %u MHz", name, &mhz) == 2) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_st.channel, sizeof(s_st.channel), "%s", name);
        s_st.mhz = (uint16_t)mhz;
        portEXIT_CRITICAL(&s_lock);
        if (s_chan) s_chan(name, (uint16_t)mhz, s_ctx);
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    for (;;) {
        const int n = uart_read_bytes(FPV_C5_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(5));
        if (n <= 0) continue;
        if (s_out) s_out(buf, (size_t)n, s_ctx);
        uint32_t lines = 0;
        for (int i = 0; i < n; ++i) {
            const char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (s_line_len) { s_line[s_line_len] = 0; parse_line(s_line); ++lines; }
                s_line_len = 0;
            } else if (s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_st.lines += lines;
        s_st.last_rx_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t c5_link_start(c5_output_cb_t out, c5_channel_cb_t chan, void *ctx)
{
    s_out = out;
    s_chan = chan;
    s_ctx = ctx;
    /* CHIP_PU is shared with the carrier's RS2233 download switch: drive it
     * open-drain so the switch path (C5 flashing via the CH343) still works. */
    const gpio_config_t en = {
        .pin_bit_mask = 1ull << FPV_C5_EN_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&en), TAG, "en gpio");
    gpio_set_level(FPV_C5_EN_GPIO, 0);

    const uart_config_t cfg = {
        .baud_rate = FPV_C5_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(FPV_C5_UART_NUM, 8192, 1024, 0, NULL, 0), TAG, "uart");
    ESP_RETURN_ON_ERROR(uart_param_config(FPV_C5_UART_NUM, &cfg), TAG, "uart cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(FPV_C5_UART_NUM, FPV_C5_UART_TX_GPIO, FPV_C5_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    xTaskCreatePinnedToCore(link_task, "c5_link", 4096, NULL, 9, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(FPV_C5_EN_GPIO, 1);
    ESP_LOGI(TAG, "C5VRX console on UART%d RX%d TX%d %d baud, EN GPIO%d", FPV_C5_UART_NUM,
             FPV_C5_UART_RX_GPIO, FPV_C5_UART_TX_GPIO, FPV_C5_UART_BAUD, FPV_C5_EN_GPIO);
    return ESP_OK;
}

void c5_link_write(const void *data, size_t len)
{
    uart_write_bytes(FPV_C5_UART_NUM, data, len);
}

void c5_link_get_status(c5_status_t *st)
{
    portENTER_CRITICAL(&s_lock);
    *st = s_st;
    portEXIT_CRITICAL(&s_lock);
    st->alive = st->last_rx_us && esp_timer_get_time() - st->last_rx_us < 6000000;
}

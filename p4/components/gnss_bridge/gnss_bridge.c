/* L76K GNSS <-> USB CDC bridge (see gnss_bridge.h). */
#include "gnss_bridge.h"
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fpv_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_composite.h"

static const char *TAG = "gnss";
static gnss_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_line[96];
static size_t s_line_len;

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    s_st.last_pps_us = esp_timer_get_time();
    s_st.pps_count++;
}

/* Light GGA parsing for the status display only; data is never altered. */
static void parse_line(const char *l)
{
    if (strlen(l) < 6 || l[0] != '$' || strncmp(l + 3, "GGA", 3) != 0) return;
    int field = 0;
    const char *p = l;
    uint8_t q = 0, sats = 0;
    while ((p = strchr(p, ',')) != NULL) {
        ++p;
        ++field;
        if (field == 6) q = (uint8_t)atoi(p);
        if (field == 7) { sats = (uint8_t)atoi(p); break; }
    }
    portENTER_CRITICAL(&s_lock);
    s_st.fix_quality = q;
    s_st.satellites = sats;
    portEXIT_CRITICAL(&s_lock);
}

static void scan(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const char c = (char)d[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len) { s_line[s_line_len] = 0; parse_line(s_line); }
            s_line_len = 0;
        } else if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

static void on_usb_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    uart_write_bytes(FPV_GNSS_UART_NUM, data, len);
    portENTER_CRITICAL(&s_lock);
    s_st.bytes_to_gnss += len;
    portEXIT_CRITICAL(&s_lock);
}

static void on_usb_baud(uint32_t baud, void *ctx)
{
    (void)ctx;
    if (baud < 1200 || baud > 921600 || baud == s_st.baud) return;
    uart_set_baudrate(FPV_GNSS_UART_NUM, baud);
    s_st.baud = baud;
    ESP_LOGI(TAG, "baud %lu (host)", (unsigned long)baud);
}

static void gnss_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        /* The read timeout batches bytes into USB packets (NMEA arrives in
         * bursts once per epoch); a few ms of delay are irrelevant. */
        const int n = uart_read_bytes(FPV_GNSS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (n <= 0) continue;
        size_t off = 0;
        for (int tries = 0; off < (size_t)n && tries < 20; ++tries) {
            off += usb_cdc_write(USB_CDC_GNSS, buf + off, (size_t)n - off);
            if (off < (size_t)n) {
                if (!usb_cdc_connected(USB_CDC_GNSS)) break;   /* nobody reading: drop */
                vTaskDelay(1);
            }
        }
        usb_cdc_flush(USB_CDC_GNSS);
        scan(buf, (size_t)n);
        portENTER_CRITICAL(&s_lock);
        s_st.bytes_from_gnss += (uint64_t)n;
        s_st.seen = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t gnss_bridge_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = FPV_GNSS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(FPV_GNSS_UART_NUM, 4096, 1024, 0, NULL, 0), TAG, "uart");
    ESP_RETURN_ON_ERROR(uart_param_config(FPV_GNSS_UART_NUM, &cfg), TAG, "uart cfg");
    ESP_RETURN_ON_ERROR(uart_set_pin(FPV_GNSS_UART_NUM, FPV_GNSS_TX_GPIO, FPV_GNSS_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    s_st.baud = FPV_GNSS_BAUD;

    const gpio_config_t pps = {
        .pin_bit_mask = 1ull << FPV_GNSS_PPS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&pps);
    gpio_install_isr_service(0);    /* may already be installed */
    gpio_isr_handler_add(FPV_GNSS_PPS_GPIO, pps_isr, NULL);

    usb_cdc_set_rx_cb(USB_CDC_GNSS, on_usb_rx, NULL);
    usb_cdc_set_baud_cb(USB_CDC_GNSS, on_usb_baud, NULL);
    xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "L76K on UART%d RX%d TX%d %d baud, PPS GPIO%d", FPV_GNSS_UART_NUM, FPV_GNSS_RX_GPIO,
             FPV_GNSS_TX_GPIO, FPV_GNSS_BAUD, FPV_GNSS_PPS_GPIO);
    return ESP_OK;
}

void gnss_bridge_get_status(gnss_status_t *st)
{
    portENTER_CRITICAL(&s_lock);
    *st = s_st;
    portEXIT_CRITICAL(&s_lock);
}

/* ESP32-C5 (C5VRX) control: reset line and console UART (see fpv_board.h). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     alive;             /* console output seen in the last 6 s */
    char     channel[12];       /* e.g. "R1" */
    uint16_t mhz;
    uint32_t lines;             /* console lines received */
    uint32_t resets;
    int64_t  last_rx_us;
} c5_status_t;

/* Raw console bytes from the C5 (c5_link task context). */
typedef void (*c5_output_cb_t)(const uint8_t *data, size_t len, void *ctx);
/* Channel change parsed from the console. */
typedef void (*c5_channel_cb_t)(const char *name, uint16_t mhz, void *ctx);

/* Holds the C5 in reset, configures the UART, then releases it. */
esp_err_t c5_link_start(c5_output_cb_t out, c5_channel_cb_t chan, void *ctx);
void c5_link_reset(void);
/* Bytes to the C5VRX console (single-key commands, see C5VRX README). */
void c5_link_write(const void *data, size_t len);
void c5_link_get_status(c5_status_t *st);

/* Continuous capture of the C5VRX digital video bus with PARLIO RX.
 * Samples land in a DMA ring of 4032-byte nodes; the consumer pulls whole
 * nodes in order. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t chunks;        /* nodes delivered by the DMA */
    uint64_t bytes;
    uint32_t overruns;      /* consumer too slow: nodes skipped */
    uint32_t timeouts;      /* no data (bus clock stopped) */
    uint32_t sample_hz;     /* unique samples per second delivered to the consumer */
    bool     decimate_sw;   /* consumer must keep every second byte */
} vrx_capture_stats_t;

esp_err_t vrx_capture_start(void);

/* Next captured node. Returns false on timeout. *discontinuity is set when
 * nodes were lost since the previous call. The data stays valid until the
 * next call (the ring holds several nodes of slack). */
bool vrx_capture_read(const uint8_t **data, size_t *len, bool *discontinuity, TickType_t wait);

void vrx_capture_get_stats(vrx_capture_stats_t *out);

#ifdef __cplusplus
}
#endif

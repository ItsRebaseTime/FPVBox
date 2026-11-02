/* PARLIO RX capture of the C5VRX digital video bus (see fpv_board.h). */
#include "vrx_capture.h"
#include <string.h>
#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fpv_board.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "vrx_capture";

/* The driver splits a ring of whole 4032-byte nodes evenly, and the soft
 * delimiter raises EOF every 4032 bytes: one callback per node. */
#define NODE_BYTES      4032u
#define RING_NODES      CONFIG_FPV_CAPTURE_RING_NODES
#define SLOTS           32u     /* power of two */

typedef struct {
    const uint8_t *data;
    uint32_t len;
} slot_t;

static uint8_t *s_ring;
static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_delim;
static TaskHandle_t s_consumer;
static slot_t s_slots[SLOTS];
static volatile uint32_t s_produced;
static uint32_t s_consumed;
static vrx_capture_stats_t s_stats;

static bool IRAM_ATTR on_partial(parlio_rx_unit_handle_t rx, const parlio_rx_event_data_t *ev, void *arg)
{
    (void)rx; (void)arg;
    const uint32_t p = s_produced;
    s_slots[p & (SLOTS - 1u)] = (slot_t){ .data = ev->data, .len = ev->recv_bytes };
    s_produced = p + 1u;
    BaseType_t woken = pdFALSE;
    if (s_consumer) vTaskNotifyGiveFromISR(s_consumer, &woken);
    return woken == pdTRUE;
}

esp_err_t vrx_capture_start(void)
{
    const size_t ring_bytes = (size_t)NODE_BYTES * RING_NODES;
    s_ring = heap_caps_aligned_alloc(64, ring_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring");

    parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1,
        .max_recv_size = ring_bytes,
        .dma_burst_size = 64,
        .data_width = 8,
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = FPV_VIDEO_BUS_HZ,
#if CONFIG_FPV_CAPTURE_HW_DECIMATE
        .exp_clk_freq_hz = FPV_VIDEO_BUS_HZ / 2u,
#else
        .exp_clk_freq_hz = FPV_VIDEO_BUS_HZ,
#endif
        .clk_in_gpio_num = FPV_VIDEO_CLK_GPIO,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .data_gpio_nums = {
            FPV_VIDEO_D0_GPIO, FPV_VIDEO_D1_GPIO, FPV_VIDEO_D2_GPIO,
            FPV_VIDEO_D3_GPIO, FPV_VIDEO_D4_GPIO, FPV_VIDEO_D5_GPIO, -1, -1,
        },
        .flags = { .free_clk = 1 },
    };
    ESP_RETURN_ON_ERROR(parlio_new_rx_unit(&cfg, &s_rx), TAG, "rx unit");

    /* C5VRX shifts on the falling edge: sample on the rising edge. */
    const parlio_rx_soft_delimiter_config_t dcfg = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = NODE_BYTES,
        .timeout_ticks = 0,
    };
    ESP_RETURN_ON_ERROR(parlio_new_rx_soft_delimiter(&dcfg, &s_delim), TAG, "delimiter");
    const parlio_rx_event_callbacks_t cbs = { .on_partial_receive = on_partial };
    ESP_RETURN_ON_ERROR(parlio_rx_unit_register_event_callbacks(s_rx, &cbs, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(parlio_rx_unit_enable(s_rx, true), TAG, "enable");
    ESP_RETURN_ON_ERROR(parlio_rx_soft_delimiter_start_stop(s_rx, s_delim, true), TAG, "start");
    const parlio_receive_config_t rcfg = {
        .delimiter = s_delim,
        .flags = { .partial_rx_en = 1, .indirect_mount = 0 },
    };
    ESP_RETURN_ON_ERROR(parlio_rx_unit_receive(s_rx, s_ring, ring_bytes, &rcfg), TAG, "receive");

#if CONFIG_FPV_CAPTURE_HW_DECIMATE
    s_stats.sample_hz = FPV_VIDEO_SAMPLE_HZ;
    s_stats.decimate_sw = false;
#else
    s_stats.sample_hz = FPV_VIDEO_SAMPLE_HZ;
    s_stats.decimate_sw = true;
#endif
    ESP_LOGI(TAG, "PARLIO RX: clk GPIO%d %u Hz%s, ring %u x %u B", FPV_VIDEO_CLK_GPIO,
             (unsigned)FPV_VIDEO_BUS_HZ, CONFIG_FPV_CAPTURE_HW_DECIMATE ? " /2" : "",
             (unsigned)RING_NODES, (unsigned)NODE_BYTES);
    return ESP_OK;
}

bool vrx_capture_read(const uint8_t **data, size_t *len, bool *discontinuity, TickType_t wait)
{
    if (!s_consumer) s_consumer = xTaskGetCurrentTaskHandle();
    *discontinuity = false;
    while (s_consumed == s_produced) {
        if (!ulTaskNotifyTake(pdTRUE, wait)) { s_stats.timeouts++; return false; }
    }
    const uint32_t lag = s_produced - s_consumed;
    /* Keep two nodes of slack between the reader and the DMA writer. */
    if (lag > RING_NODES - 2u || lag > SLOTS - 1u) {
        s_stats.overruns += lag - 1u;
        s_consumed = s_produced - 1u;
        *discontinuity = true;
    }
    const slot_t sl = s_slots[s_consumed & (SLOTS - 1u)];
    s_consumed++;
    s_stats.chunks++;
    s_stats.bytes += sl.len;
    *data = sl.data;
    *len = sl.len;
    return true;
}

void vrx_capture_get_stats(vrx_capture_stats_t *out) { *out = s_stats; }

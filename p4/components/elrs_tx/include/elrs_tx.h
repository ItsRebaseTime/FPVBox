/* ExpressLRS 2.4 GHz TX on the LoRa shield's SX1280 + PA, driven over the
 * USB CDC "ELRS" port with CRSF (like a radio talks to an ELRS TX module):
 *   host -> box : RC channels (0x16), parameter reads/writes, commands
 *   box  -> host: link statistics (0x14), all downlink telemetry from the
 *                 receiver (GPS, battery, attitude, flight mode, MSP ...)
 * One radio does both directions; no second radio is involved. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ELRS_STATE_OFF = 0,         /* not started / radio missing */
    ELRS_STATE_NO_HOST,         /* no CRSF channel frames from the USB host */
    ELRS_STATE_DISCONNECTED,    /* transmitting, no telemetry from the receiver */
    ELRS_STATE_CONNECTED,       /* link up */
    ELRS_STATE_BINDING,
    ELRS_STATE_FAULT,
} elrs_tx_state_t;

typedef struct {
    elrs_tx_state_t state;
    uint16_t rate_hz;           /* packet rate */
    int8_t   power_dbm;
    uint8_t  uplink_lq;         /* % as reported by the receiver */
    int8_t   downlink_rssi;     /* dBm at the box */
    int8_t   downlink_snr;
    uint32_t host_frames;       /* CRSF frames from the host */
    uint32_t tx_packets;
    uint32_t telemetry_frames;  /* CRSF frames sent to the host */
} elrs_tx_status_t;

esp_err_t elrs_tx_start(void);
void elrs_tx_get_status(elrs_tx_status_t *st);
const char *elrs_tx_state_name(elrs_tx_state_t s);
/* Text command from the control port ("help" lists them). */
void elrs_tx_command(const char *cmd, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

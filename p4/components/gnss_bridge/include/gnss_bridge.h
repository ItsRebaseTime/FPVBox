/* L76K GNSS <-> USB CDC "GNSS" port, byte for byte. The port behaves like a
 * USB-UART adapter: the host's baud rate setting is applied to the UART, so
 * gpsd and similar tools can reconfigure the receiver ($PCAS01). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t baud;
    uint64_t bytes_from_gnss;
    uint64_t bytes_to_gnss;
    uint32_t pps_count;
    int64_t  last_pps_us;       /* esp_timer time of the last PPS edge */
    uint8_t  fix_quality;       /* GGA field 6 (0 = no fix) */
    uint8_t  satellites;        /* GGA field 7 */
    bool     seen;              /* any NMEA received */
} gnss_status_t;

esp_err_t gnss_bridge_start(void);
void gnss_bridge_get_status(gnss_status_t *st);

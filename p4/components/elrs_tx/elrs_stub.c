/* Placeholder until the ExpressLRS port is in place. */
#include <stdio.h>
#include "elrs_tx.h"

esp_err_t elrs_tx_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void elrs_tx_get_status(elrs_tx_status_t *st) { *st = (elrs_tx_status_t){ .state = ELRS_STATE_OFF }; }
const char *elrs_tx_state_name(elrs_tx_state_t s)
{
    static const char *const n[] = {"off", "no-host", "disconnected", "connected", "binding", "fault"};
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}
void elrs_tx_command(const char *cmd, char *out, size_t out_len) { (void)cmd; snprintf(out, out_len, "elrs not built"); }

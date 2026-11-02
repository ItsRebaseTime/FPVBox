/* FPVBox <-> ExpressLRS glue: tasks, status and control commands. */
#include "elrs_tx.h"
#include "targets.h"
#include "common.h"
#include "config.h"
#include "OTA.h"
#include "POWERMGNT.h"
#include "elrs_port.h"
#include "esp_log.h"
#include "freertos/queue.h"

static const char *TAG = "elrs";

/* ExpressLRS tx_main.cpp */
void setup();
void loop();
void EnterBindingModeSafely();
extern TxConfig config;

/* ExpressLRS runs its loop and "ISRs" on core 1 like on a classic ESP32
 * (its device framework puts a helper task on core 0). The loop is paced by
 * USB input or 1 ms and sits above the video decoder workers. */
#define ELRS_CORE           1
#define ELRS_RT_PRIO        24
#define ELRS_LOOP_PRIO      21

typedef struct { char text[48]; } cmd_t;
static QueueHandle_t s_cmds;
static volatile bool s_running;

static void run_cmd(const char *c)
{
    int v;
    if (!strcmp(c, "bind")) {
        EnterBindingModeSafely();
    } else if (sscanf(c, "power %d", &v) == 1) {
        config.SetPower((uint8_t)v);
        config.Commit();
    } else if (sscanf(c, "rate %d", &v) == 1) {
        config.SetRate((uint8_t)v);
        config.Commit();
    } else if (sscanf(c, "tlm %d", &v) == 1) {
        config.SetTlm((uint8_t)v);
        config.Commit();
    }
}

static void elrs_task(void *arg)
{
    (void)arg;
    usb_handset_attach();       /* this task is woken by USB input */
    setup();
    s_running = true;
    ESP_LOGI(TAG, "ExpressLRS TX running, state %d", (int)connectionState);
    for (;;) {
        loop();
        cmd_t cmd;
        while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) run_cmd(cmd.text);
        ulTaskNotifyTake(pdTRUE, 1);
    }
}

esp_err_t elrs_tx_start(void)
{
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    if (!s_cmds) return ESP_ERR_NO_MEM;
    elrs_rt_start(ELRS_CORE, ELRS_RT_PRIO);
    if (xTaskCreatePinnedToCore(elrs_task, "elrs", 16384, NULL, ELRS_LOOP_PRIO, NULL, ELRS_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static int8_t power_dbm(PowerLevels_e p)
{
    static const int8_t dbm[] = {10, 14, 17, 20, 24, 27, 30, 33};
    return (unsigned)p < sizeof(dbm) ? dbm[p] : 0;
}

void elrs_tx_get_status(elrs_tx_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!s_running) { st->state = ELRS_STATE_OFF; return; }
    switch (connectionState) {
    case connected: st->state = ELRS_STATE_CONNECTED; break;
    case disconnected:
    case tentative:
    case awaitingModelId: st->state = ELRS_STATE_DISCONNECTED; break;
    case noCrossfire: st->state = ELRS_STATE_NO_HOST; break;
    case radioFailed:
    case hardwareUndefined: st->state = ELRS_STATE_FAULT; break;
    default: st->state = ELRS_STATE_DISCONNECTED; break;
    }
    if (InBindingMode) st->state = ELRS_STATE_BINDING;
    if (ExpressLRS_currAirRate_Modparams && ExpressLRS_currAirRate_Modparams->interval > 0)
        st->rate_hz = (uint16_t)(1000000 / ExpressLRS_currAirRate_Modparams->interval);
    st->power_dbm = power_dbm(POWERMGNT::currPower());
    st->uplink_lq = linkStats.uplink_Link_quality;
    st->downlink_rssi = (int8_t)-(int)linkStats.downlink_RSSI_1;
    st->downlink_snr = linkStats.downlink_SNR;
    st->host_frames = usb_handset_host_frames();
    st->telemetry_frames = usb_handset_frames_to_host();
}

const char *elrs_tx_state_name(elrs_tx_state_t s)
{
    static const char *const n[] = {"off", "no-host", "disconnected", "connected", "binding", "fault"};
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

void elrs_tx_command(const char *cmd, char *out, size_t out_len)
{
    if (!cmd[0] || !strcmp(cmd, "help")) {
        snprintf(out, out_len, "elrs: bind | power <0..%d> | rate <idx> | tlm <idx> (also via CRSF parameters)",
                 CONFIG_FPV_ELRS_MAX_POWER_IDX);
        return;
    }
    if (!s_running) { snprintf(out, out_len, "elrs not running"); return; }
    cmd_t c = {};
    snprintf(c.text, sizeof(c.text), "%s", cmd);
    snprintf(out, out_len, xQueueSend(s_cmds, &c, 0) == pdTRUE ? "queued: %s" : "busy: %s", cmd);
}

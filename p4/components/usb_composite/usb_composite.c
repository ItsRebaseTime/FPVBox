/* FPVBox USB composite device runtime (see usb_composite.h). */
#include "usb_composite.h"
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_descriptors.h"

static const char *TAG = "usb";

static usb_phy_handle_t s_phy;
static usb_video_callbacks_t s_video;
static volatile bool s_video_busy;
static bool s_video_started;
static struct {
    usb_cdc_rx_cb_t cb;
    void *ctx;
    usb_cdc_baud_cb_t baud_cb;
    void *baud_ctx;
} s_rx[USB_CDC_COUNT];

const char *fpv_usb_serial_string(void)
{
    static char serial[13];
    if (!serial[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return serial;
}

/* ---------------------------------------------------------------------- */
/* Video                                                                    */

static void video_stop(void)
{
    if (s_video_started && s_video.stop) s_video.stop(s_video.ctx);
    s_video_started = false;
    s_video_busy = false;
}

/* Host closed a bulk stream without SET_INTERFACE: drop the stale transfer. */
static void reset_video_ep(void)
{
    usbd_edpt_stall(0, EP_VIDEO);
    usbd_edpt_clear_stall(0, EP_VIDEO);
    usbd_edpt_release(0, EP_VIDEO);
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, video_probe_and_commit_control_t const *p)
{
    (void)ctl_idx; (void)stm_idx;
    usb_video_stream_t st = { .interval_100ns = p->dwFrameInterval };
    const usb_frame_t *frm = NULL;
    switch (p->bFormatIndex) {
    case 1:
        st.format = USB_VIDEO_YUYV;
        if (p->bFrameIndex >= 1 && p->bFrameIndex <= 4) frm = &g_uvc_frames_yuyv[p->bFrameIndex - 1];
        break;
    case 2:
    case 3:
        st.format = p->bFormatIndex == 2 ? USB_VIDEO_MJPEG : USB_VIDEO_H264;
        if (p->bFrameIndex >= 1 && p->bFrameIndex <= 2) frm = &g_uvc_frames_coded[p->bFrameIndex - 1];
        break;
    default:
        break;
    }
    if (!frm) return VIDEO_ERROR_OUT_OF_RANGE;
    st.width = frm->width;
    st.height = frm->height;
    if (!st.interval_100ns) st.interval_100ns = frm->interval;

    video_stop();
    reset_video_ep();
    if (s_video.start) s_video.start(&st, s_video.ctx);
    s_video_started = true;
    return VIDEO_ERROR_NONE;
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)ctl_idx; (void)stm_idx;
    s_video_busy = false;
    if (s_video.xfer_done) s_video.xfer_done(s_video.ctx);
}

bool usb_video_is_streaming(void) { return s_video_started && tud_video_n_streaming(0, 0); }

bool usb_video_send(const void *buf, size_t len)
{
    if (!usb_video_is_streaming() || s_video_busy) return false;
    s_video_busy = true;
    if (!tud_video_n_frame_xfer(0, 0, (void *)buf, len)) {
        s_video_busy = false;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* CDC                                                                      */

void tud_cdc_rx_cb(uint8_t itf)
{
    uint8_t buf[512];
    while (tud_cdc_n_available(itf)) {
        const uint32_t n = tud_cdc_n_read(itf, buf, sizeof(buf));
        if (!n) break;
        if (itf < USB_CDC_COUNT && s_rx[itf].cb) s_rx[itf].cb(buf, n, s_rx[itf].ctx);
    }
}

size_t usb_cdc_write(usb_cdc_port_t port, const void *data, size_t len)
{
    if (port >= USB_CDC_COUNT || !tud_mounted()) return 0;
    return tud_cdc_n_write(port, data, (uint32_t)len);
}

void usb_cdc_flush(usb_cdc_port_t port)
{
    if (port < USB_CDC_COUNT && tud_mounted()) tud_cdc_n_write_flush(port);
}

void usb_cdc_set_rx_cb(usb_cdc_port_t port, usb_cdc_rx_cb_t cb, void *ctx)
{
    if (port >= USB_CDC_COUNT) return;
    s_rx[port].ctx = ctx;
    s_rx[port].cb = cb;
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *lc)
{
    if (itf < USB_CDC_COUNT && s_rx[itf].baud_cb) s_rx[itf].baud_cb(lc->bit_rate, s_rx[itf].baud_ctx);
}

void usb_cdc_set_baud_cb(usb_cdc_port_t port, usb_cdc_baud_cb_t cb, void *ctx)
{
    if (port >= USB_CDC_COUNT) return;
    s_rx[port].baud_ctx = ctx;
    s_rx[port].baud_cb = cb;
}

bool usb_cdc_connected(usb_cdc_port_t port) { return port < USB_CDC_COUNT && tud_cdc_n_connected(port); }

uint32_t usb_cdc_get_baud(usb_cdc_port_t port)
{
    if (port >= USB_CDC_COUNT) return 0;
    cdc_line_coding_t lc;
    tud_cdc_n_get_line_coding(port, &lc);
    return lc.bit_rate;
}

/* ---------------------------------------------------------------------- */

void tud_mount_cb(void) { ESP_LOGI(TAG, "mounted"); }

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "unmounted");
    video_stop();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    video_stop();
}

static void usb_task(void *arg)
{
    (void)arg;
    bool was_streaming = false;
    for (;;) {
        tud_task_ext(10, false);
        /* A host that closes the stream (SET_INTERFACE alt 0 or unplug)
         * ends streaming without a commit callback. */
        const bool streaming = s_video_started && tud_video_n_streaming(0, 0);
        if (was_streaming && !streaming) video_stop();
        was_streaming = streaming;
    }
}

esp_err_t usb_composite_start(const usb_video_callbacks_t *video)
{
    if (video) s_video = *video;
    const usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
        .otg_speed = USB_PHY_SPEED_HIGH,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy), TAG, "phy");
    ESP_RETURN_ON_FALSE(tusb_init(), ESP_FAIL, TAG, "tusb_init");
    xTaskCreatePinnedToCore(usb_task, "tinyusb", 6144, NULL, 21, NULL, CONFIG_FPV_USB_TASK_CORE);
    ESP_LOGI(TAG, "composite device %04x:%04x serial %s", CONFIG_FPV_USB_VID, CONFIG_FPV_USB_PID,
             fpv_usb_serial_string());
    return ESP_OK;
}

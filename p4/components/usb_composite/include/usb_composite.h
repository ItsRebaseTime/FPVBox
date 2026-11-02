/* FPVBox USB 2.0 high-speed composite device (ESP32-P4 OTG HS, Type-C J8):
 *   - UVC 1.5 camera: YUYV (raw), MJPEG, H.264 (frame-based), bulk endpoint
 *   - CDC-ACM 0 "ELRS"    CRSF frames to/from the ELRS TX
 *   - CDC-ACM 1 "GNSS"    raw NMEA from/to the L76K
 *   - CDC-ACM 2 "Control" box console + C5VRX console passthrough
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_CDC_ELRS = 0,
    USB_CDC_GNSS = 1,
    USB_CDC_CTRL = 2,
    USB_CDC_COUNT
} usb_cdc_port_t;

typedef enum {
    USB_VIDEO_YUYV = 0,
    USB_VIDEO_MJPEG,
    USB_VIDEO_H264,
} usb_video_format_t;

typedef struct {
    usb_video_format_t format;
    uint16_t width, height;
    uint32_t interval_100ns;    /* frame interval, UVC units */
} usb_video_stream_t;

typedef struct {
    /* Host committed a stream (TinyUSB task context; must not block). */
    void (*start)(const usb_video_stream_t *s, void *ctx);
    /* Host stopped streaming or the device was unplugged. */
    void (*stop)(void *ctx);
    /* The buffer given to usb_video_send() has been sent completely. */
    void (*xfer_done)(void *ctx);
    void *ctx;
} usb_video_callbacks_t;

typedef void (*usb_cdc_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t usb_composite_start(const usb_video_callbacks_t *video);

/* Start sending one video frame. The buffer must stay untouched until
 * xfer_done. False when not streaming or a transfer is still running. */
bool usb_video_send(const void *buf, size_t len);
bool usb_video_is_streaming(void);

/* Non-blocking: returns the bytes queued (0 when the host is not reading). */
size_t usb_cdc_write(usb_cdc_port_t port, const void *data, size_t len);
void usb_cdc_flush(usb_cdc_port_t port);
/* Received data, called from the TinyUSB task. */
void usb_cdc_set_rx_cb(usb_cdc_port_t port, usb_cdc_rx_cb_t cb, void *ctx);
/* Host changed the port's line coding (baud rate), TinyUSB task context. */
typedef void (*usb_cdc_baud_cb_t)(uint32_t baud, void *ctx);
void usb_cdc_set_baud_cb(usb_cdc_port_t port, usb_cdc_baud_cb_t cb, void *ctx);
/* DTR set by the host (a terminal/program has the port open). */
bool usb_cdc_connected(usb_cdc_port_t port);
/* Baud rate the host configured (informational; CDC is not rate-limited). */
uint32_t usb_cdc_get_baud(usb_cdc_port_t port);

#ifdef __cplusplus
}
#endif

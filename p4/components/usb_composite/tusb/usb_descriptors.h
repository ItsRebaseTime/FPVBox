/* FPVBox composite device: interface, endpoint and string numbering. */
#pragma once
#include <stdint.h>

enum {
    ITF_VC = 0,
    ITF_VS,
    ITF_CDC_ELRS,           /* + data interface */
    ITF_CDC_ELRS_DATA,
    ITF_CDC_GNSS,
    ITF_CDC_GNSS_DATA,
    ITF_CDC_CTRL,
    ITF_CDC_CTRL_DATA,
    ITF_COUNT
};

#define EP_VIDEO        0x81
#define EP_ELRS_NOTIF   0x82
#define EP_ELRS_OUT     0x03
#define EP_ELRS_IN      0x83
#define EP_GNSS_NOTIF   0x84
#define EP_GNSS_OUT     0x05
#define EP_GNSS_IN      0x85
#define EP_CTRL_NOTIF   0x86
#define EP_CTRL_OUT     0x07
#define EP_CTRL_IN      0x87

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_VIDEO,
    STRID_CDC_ELRS,
    STRID_CDC_GNSS,
    STRID_CDC_CTRL,
};

#define UVC_ENTITY_CAMERA   1
#define UVC_ENTITY_OUTPUT   2

/* Frame intervals, 100 ns units. */
#define UVC_IV_5994     166833u     /* 59.94 fields/s */
#define UVC_IV_50       200000u
#define UVC_IV_2997     333667u
#define UVC_IV_25       400000u

typedef struct {
    uint16_t width, height;
    uint32_t interval;
} usb_frame_t;

/* Format 1 (YUYV) frames 1..4; formats 2 (MJPEG) and 3 (H.264) frames 1..2. */
extern const usb_frame_t g_uvc_frames_yuyv[4];
extern const usb_frame_t g_uvc_frames_coded[2];

/* Serial number string (chip MAC), provided by usb_composite.c. */
const char *fpv_usb_serial_string(void);

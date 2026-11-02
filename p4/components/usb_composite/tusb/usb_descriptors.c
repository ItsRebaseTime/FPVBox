/* FPVBox composite device descriptors: UVC (YUYV, MJPEG, H.264) + 3x CDC-ACM.
 * Compiled into the TinyUSB library (see CMakeLists.txt). */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

/* ---------------------------------------------------------------------- */
/* Device                                                                   */

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD composite */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_FPV_USB_VID,
    .idProduct = CONFIG_FPV_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&s_device; }

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

uint8_t const *tud_descriptor_device_qualifier_cb(void) { return (uint8_t const *)&s_qualifier; }

/* ---------------------------------------------------------------------- */
/* Video streaming formats                                                  */

/* Frame descriptors with one discrete interval (TinyUSB's *_DISC macros take
 * the interval list as bytes but size it as 32-bit values). */
#define FRM_DISC1_LEN   30
#define FRM_DISC1(_subtype, _idx, _w, _h, _minbr, _maxbr, _bufsz, _iv) \
    FRM_DISC1_LEN, TUSB_DESC_CS_INTERFACE, _subtype, _idx, 0, U16_TO_U8S_LE(_w), U16_TO_U8S_LE(_h), \
    U32_TO_U8S_LE(_minbr), U32_TO_U8S_LE(_maxbr), U32_TO_U8S_LE(_bufsz), U32_TO_U8S_LE(_iv), 1, U32_TO_U8S_LE(_iv)
/* Frame-based frames: default interval, count, dwBytesPerLine (0: variable size). */
#define FRM_FB_DISC1(_idx, _w, _h, _minbr, _maxbr, _iv) \
    FRM_DISC1_LEN, TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VS_FRAME_FRAME_BASED, _idx, 0, U16_TO_U8S_LE(_w), \
    U16_TO_U8S_LE(_h), U32_TO_U8S_LE(_minbr), U32_TO_U8S_LE(_maxbr), U32_TO_U8S_LE(_iv), 1, U32_TO_U8S_LE(0), \
    U32_TO_U8S_LE(_iv)

#define W               720
#define BR(w, h, bpp, iv)   ((uint32_t)((uint64_t)(w) * (h) * (bpp) * 10000000ull / (iv)))
#define YUY2_FRM_LEN    FRM_DISC1_LEN
#define MJPEG_FRM_LEN   FRM_DISC1_LEN
#define H264_FRM_LEN    FRM_DISC1_LEN
#define FMT_YUY2_LEN    (TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN + 4 * YUY2_FRM_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN)
#define FMT_MJPEG_LEN   (TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + 2 * MJPEG_FRM_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN)
#define FMT_H264_LEN    (TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED_LEN + 2 * H264_FRM_LEN + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN)
#define VS_IN_LEN       (TUD_VIDEO_DESC_CS_VS_IN_LEN + 3)      /* bmaControls: one byte per format */
#define VS_TOTAL        (VS_IN_LEN + FMT_YUY2_LEN + FMT_MJPEG_LEN + FMT_H264_LEN)
#define VC_UNITS_LEN    (TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN)

#define UVC_LEN         (TUD_VIDEO_DESC_IAD_LEN + TUD_VIDEO_DESC_STD_VC_LEN + (TUD_VIDEO_DESC_CS_VC_LEN + 1) + \
                         VC_UNITS_LEN + TUD_VIDEO_DESC_STD_VS_LEN + VS_TOTAL + 7)
#define CONFIG_LEN      (TUD_CONFIG_DESC_LEN + UVC_LEN + 3 * TUD_CDC_DESC_LEN)

/* BT.601 colour: primaries SMPTE 170M, BT.709 transfer, SMPTE 170M matrix. */
#define COLOR_601       TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(4, 1, 4)

/* Frame tables shared with usb_composite.c. */
const usb_frame_t g_uvc_frames_yuyv[4] = {
    { W, 240, UVC_IV_5994 }, { W, 288, UVC_IV_50 }, { W, 480, UVC_IV_2997 }, { W, 576, UVC_IV_25 },
};
const usb_frame_t g_uvc_frames_coded[2] = {
    { W, 480, UVC_IV_5994 }, { W, 576, UVC_IV_50 },
};

static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0x80, 500),

    /* ---- UVC ---- */
    TUD_VIDEO_DESC_IAD(ITF_VC, 2, STRID_VIDEO),
    TUD_VIDEO_DESC_STD_VC(ITF_VC, 0, STRID_VIDEO),
    TUD_VIDEO_DESC_CS_VC(0x0150, VC_UNITS_LEN, 27000000, ITF_VS),
    TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAMERA, 0, 0, 0, 0, 0, 0),
    TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_OUTPUT, VIDEO_TT_STREAMING, 0, UVC_ENTITY_CAMERA, 0),
    TUD_VIDEO_DESC_STD_VS(ITF_VS, 0, 1, STRID_VIDEO),
    /* VS input header for three formats (written out: TinyUSB's macro
     * emits only one bmaControls entry). */
    VS_IN_LEN, TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VS_INPUT_HEADER, 3,
    U16_TO_U8S_LE(VS_TOTAL), EP_VIDEO, 0, UVC_ENTITY_OUTPUT, 0, 0, 0, 1, 0, 0, 0,

    /* Format 1: YUY2. Fields (lowest latency) and woven frames. */
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(1, 4, TUD_VIDEO_GUID_YUY2, 16, 1, 4, 3, 0, 0),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_UNCOMPRESSED, 1, W, 240, BR(W, 240, 16, UVC_IV_5994), BR(W, 240, 16, UVC_IV_5994),
              W * 240 * 2, UVC_IV_5994),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_UNCOMPRESSED, 2, W, 288, BR(W, 288, 16, UVC_IV_50), BR(W, 288, 16, UVC_IV_50),
              W * 288 * 2, UVC_IV_50),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_UNCOMPRESSED, 3, W, 480, BR(W, 480, 16, UVC_IV_2997), BR(W, 480, 16, UVC_IV_2997),
              W * 480 * 2, UVC_IV_2997),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_UNCOMPRESSED, 4, W, 576, BR(W, 576, 16, UVC_IV_25), BR(W, 576, 16, UVC_IV_25),
              W * 576 * 2, UVC_IV_25),
    COLOR_601,

    /* Format 2: MJPEG, field-rate progressive frames. */
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(2, 2, 0, 1, 4, 3, 0, 0),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_MJPEG, 1, W, 480, BR(W, 480, 2, UVC_IV_5994), BR(W, 480, 8, UVC_IV_5994),
              W * 480 * 2, UVC_IV_5994),
    FRM_DISC1(VIDEO_CS_ITF_VS_FRAME_MJPEG, 2, W, 576, BR(W, 576, 2, UVC_IV_50), BR(W, 576, 8, UVC_IV_50),
              W * 576 * 2, UVC_IV_50),
    COLOR_601,

    /* Format 3: H.264 (frame based), field-rate progressive frames. */
    TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED(3, 2, TUD_VIDEO_GUID_H264, 16, 1, 4, 3, 0, 0, 1),
    FRM_FB_DISC1(1, W, 480, 1000000, 20000000, UVC_IV_5994),
    FRM_FB_DISC1(2, W, 576, 1000000, 20000000, UVC_IV_50),
    COLOR_601,

    TUD_VIDEO_DESC_EP_BULK(EP_VIDEO, 512, 1),

    /* ---- CDC-ACM ports ---- */
    TUD_CDC_DESCRIPTOR(ITF_CDC_ELRS, STRID_CDC_ELRS, EP_ELRS_NOTIF, 16, EP_ELRS_OUT, EP_ELRS_IN, 512),
    TUD_CDC_DESCRIPTOR(ITF_CDC_GNSS, STRID_CDC_GNSS, EP_GNSS_NOTIF, 16, EP_GNSS_OUT, EP_GNSS_IN, 512),
    TUD_CDC_DESCRIPTOR(ITF_CDC_CTRL, STRID_CDC_CTRL, EP_CTRL_NOTIF, 16, EP_CTRL_OUT, EP_CTRL_IN, 512),
};

_Static_assert(sizeof(s_config) == CONFIG_LEN, "configuration descriptor length");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_config;
}

/* ---------------------------------------------------------------------- */
/* Strings                                                                  */

static const char *const s_strings[] = {
    [STRID_MANUFACTURER] = "FPVBox",
    [STRID_PRODUCT] = "FPVBox VRX + ELRS + GNSS",
    [STRID_VIDEO] = "FPVBox Video",
    [STRID_CDC_ELRS] = "FPVBox ELRS (CRSF)",
    [STRID_CDC_GNSS] = "FPVBox GNSS (NMEA)",
    [STRID_CDC_CTRL] = "FPVBox Control",
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[48];
    size_t n;
    if (index == STRID_LANGID) {
        buf[1] = 0x0409;
        n = 1;
    } else {
        const char *s = index == STRID_SERIAL ? fpv_usb_serial_string()
                        : index < sizeof(s_strings) / sizeof(s_strings[0]) ? s_strings[index] : NULL;
        if (!s) return NULL;
        n = strlen(s);
        if (n > 47) n = 47;
        for (size_t i = 0; i < n; ++i) buf[1 + i] = (uint8_t)s[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

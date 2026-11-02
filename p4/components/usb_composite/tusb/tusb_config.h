/* TinyUSB configuration for the FPVBox composite device (ESP32-P4 OTG HS). */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "sdkconfig.h"

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (espressif/tinyusb sets it)
#endif

/* The high-speed OTG controller is root hub port 1 on the ESP32-P4. */
#define CFG_TUSB_RHPORT1_MODE       (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH        freertos/
#ifndef ESP_PLATFORM
#define ESP_PLATFORM                1
#endif
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(64)))
#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUD_DWC2_DMA_ENABLE     CONFIG_FPV_USB_DWC2_DMA

/* Classes */
#define CFG_TUD_CDC                 3
#define CFG_TUD_CDC_RX_BUFSIZE      2048
#define CFG_TUD_CDC_TX_BUFSIZE      4096
#define CFG_TUD_CDC_EP_BUFSIZE      512

#define CFG_TUD_VIDEO               1
#define CFG_TUD_VIDEO_STREAMING     1
#define CFG_TUD_VIDEO_STREAMING_BULK 1
/* One UVC payload per bulk transfer: large payloads keep the per-transfer
 * overhead low (the TinyUSB task copies each payload once). */
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE  CONFIG_FPV_UVC_PAYLOAD_BYTES

#endif

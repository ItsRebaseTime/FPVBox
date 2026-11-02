/* FPVBox pin map: OSPTEK ESP32-P4C5 module on its V1.3 carrier, plus the
 * LilyGo T-ETH-Elite LoRa shield on jumper wires. See docs/WIRING.md. */
#pragma once

/* --- On-module ESP32-C5 -> ESP32-P4 video bus (fixed traces) ------------
 * C5VRX (board profile osptek-p4c5) shifts six-bit composite samples on the
 * falling edge of a 40 MHz clock; every sample is sent twice (20 MS/s). */
#define FPV_VIDEO_CLK_GPIO      18      /* C5 GPIO9  (SDIO CLK trace) */
#define FPV_VIDEO_D0_GPIO       14      /* C5 GPIO8  (SDIO D0) */
#define FPV_VIDEO_D1_GPIO       15      /* C5 GPIO7  (SDIO D1) */
#define FPV_VIDEO_D2_GPIO       16      /* C5 GPIO14 (SDIO D2) */
#define FPV_VIDEO_D3_GPIO       17      /* C5 GPIO13 (SDIO D3) */
#define FPV_VIDEO_D4_GPIO       19      /* C5 GPIO10 (SDIO CMD) */
#define FPV_VIDEO_D5_GPIO       6       /* C5 GPIO2 */
#define FPV_VIDEO_BUS_HZ        40000000u
#define FPV_VIDEO_SAMPLE_HZ     20000000u   /* unique samples per second */

/* --- ESP32-C5 control ---------------------------------------------------- */
#define FPV_C5_EN_GPIO          54      /* C5 CHIP_PU, open drain, low = reset */
#define FPV_C5_UART_NUM         1
#define FPV_C5_UART_RX_GPIO     22      /* from C5 GPIO23 (C5VRX console TX) */
#define FPV_C5_UART_TX_GPIO     23      /* to C5 GPIO24 (C5VRX console RX) */
#define FPV_C5_UART_BAUD        2000000

/* --- SX1280 + PA ("17AP" LilyGo module on the LoRa shield) ---------------- */
#define FPV_SX_SPI_HOST         SPI2_HOST
#define FPV_SX_NSS_GPIO         28      /* shield pin 13 */
#define FPV_SX_MOSI_GPIO        29      /* shield pin 19 */
#define FPV_SX_SCK_GPIO         30      /* shield pin 23 */
#define FPV_SX_MISO_GPIO        31      /* shield pin 21 */
#define FPV_SX_DIO1_GPIO        32      /* shield pin 27 */
#define FPV_SX_BUSY_GPIO        33      /* shield pin 29 */
#define FPV_SX_RST_GPIO         27      /* shield pin 28 */
#define FPV_SX_TXEN_GPIO        46      /* shield pin 18, RADIO_EN/TX */
#define FPV_SX_RXEN_GPIO        47      /* shield pin 40, RADIO_RX */

/* --- Quectel L76K GNSS on the shield -------------------------------------- */
#define FPV_GNSS_UART_NUM       2
#define FPV_GNSS_RX_GPIO        48      /* from shield pin 16 (GPS_TX) */
#define FPV_GNSS_TX_GPIO        49      /* to shield pin 15 (GPS_RX) */
#define FPV_GNSS_PPS_GPIO       50      /* shield pin 32 via JP1 */
#define FPV_GNSS_BAUD           9600

/* --- Shield extras ------------------------------------------------------- */
#define FPV_BTN_ADC_GPIO        51      /* shield pin 31 via JP4, ADC2 */
#define FPV_I2C_SDA_GPIO        7       /* carrier I2C bus (10k pull-ups) */
#define FPV_I2C_SCL_GPIO        8

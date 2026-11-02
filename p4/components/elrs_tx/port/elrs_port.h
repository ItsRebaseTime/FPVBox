/* FPVBox ExpressLRS port internals. */
#pragma once
#include <stdint.h>

/* The real-time task: runs ExpressLRS' "ISR" work (hwTimer tock, SX1280
 * DIO1) in task context on the ExpressLRS core, so SPI may block. */
void elrs_rt_start(int core, int priority);
void elrs_rt_set_timer_cb(void (*cb)(void));
void elrs_rt_notify_timer_from_isr(void);

/* USB CRSF handset (usb_handset.cpp). */
void usb_handset_attach(void);
uint32_t usb_handset_host_frames(void);
uint32_t usb_handset_frames_to_host(void);
bool usb_handset_connected(void);

/* Hardware layout and options (options.cpp). */
void elrs_options_prepare(void);

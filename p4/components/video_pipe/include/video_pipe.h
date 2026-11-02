/* FPVBox video pipeline: C5VRX bus capture -> composite decoder (sync task
 * plus parallel line workers) -> fields -> UVC (YUYV / MJPEG / H.264). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cvbs.h"
#include "usb_composite.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Decoder */
    bool       locked;
    cvbs_std_t std;
    bool       color;
    float      line_period_us;
    float      burst_ratio;
    float      afc_ppm;
    uint32_t   lock_losses;
    uint32_t   lines_dropped;
    /* Capture */
    uint32_t   capture_overruns;
    uint32_t   capture_timeouts;
    /* Fields and output */
    uint32_t   fields_decoded;
    uint32_t   fields_dropped;      /* no free field buffer */
    uint32_t   frames_sent;
    uint32_t   frames_dropped;      /* USB still busy */
    float      out_fps;
    bool       streaming;
    usb_video_stream_t stream;
    uint32_t   last_encoded_bytes;
    float      decode_ms;           /* sync-to-field-complete latency */
} video_pipe_status_t;

esp_err_t video_pipe_start(void);

/* USB glue (usb_composite callbacks). */
const usb_video_callbacks_t *video_pipe_usb_callbacks(void);

void video_pipe_get_status(video_pipe_status_t *st);
void video_pipe_set_color(bool on, float saturation);
/* Text shown on the NO SIGNAL screen (e.g. "R1 5658 MHZ"); copied. */
void video_pipe_set_info(const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif

/* Software composite video (CVBS) decoder for the ESP32-P4.
 *
 * Input: a continuous stream of 6-bit composite samples (C5VRX digital bus,
 * 20 MS/s unique samples). Output: BT.601 YUYV lines, 720 pixels wide, one
 * field at a time (240 active lines NTSC, 288 PAL).
 *
 * Two stages, both plain C so the same code runs on the host for tests:
 *   cvbs_sync  - level tracking, H/V sync, line PLL, standard and field
 *                detection, colour burst reference and AFC. Sequential.
 *   cvbs_line  - one line: resample to 13.5 MHz, chroma demodulation,
 *                luma/chroma separation, YUYV. Independent per line, so
 *                several workers (cores) can run it in parallel.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CVBS_OUT_WIDTH          720
#define CVBS_OUT_STRIDE         (CVBS_OUT_WIDTH * 2)    /* YUYV bytes per line */
#define CVBS_MAX_FIELD_LINES    288
#define CVBS_LINE_RAW_MAX       1536    /* raw samples carried per line job */

typedef enum {
    CVBS_STD_NONE = 0,
    CVBS_STD_NTSC,      /* 525 lines, 59.94 fields/s, fsc 3.579545 MHz */
    CVBS_STD_PAL,       /* 625 lines, 50 fields/s, fsc 4.43361875 MHz */
} cvbs_std_t;

/* One active line, self-contained: everything a worker needs. */
typedef struct {
    cvbs_std_t std;
    uint8_t  field;             /* 1 or 2 */
    uint16_t line;              /* active line within the field */
    uint32_t field_seq;         /* field counter (identifies the target buffer) */
    bool     color;             /* burst present: demodulate chroma */
    float    tip, blank;        /* sync tip and blanking level, raw code units */
    float    act0;              /* raw position of output pixel 0 (rel. raw[0]) */
    float    px_step;           /* raw samples per output pixel */
    uint32_t phase0;            /* subcarrier NCO phase at raw[0] (2^32 = 1 cycle) */
    uint32_t inc;               /* NCO phase increment per raw sample */
    float    cos_psi, sin_psi;  /* chroma reference rotation (from burst) */
    float    vsign;             /* PAL V switch: +1 / -1 (NTSC: +1) */
    float    burst_amp;         /* burst amplitude, raw code units */
    float    saturation;        /* user saturation (1.0 = nominal) */
    uint16_t n;                 /* valid raw samples */
    uint8_t  raw[CVBS_LINE_RAW_MAX];
} cvbs_line_job_t;

/* Field boundary events from the sync stage. */
typedef enum {
    CVBS_EV_FIELD_START,        /* lines of field_seq follow */
    CVBS_EV_FIELD_END,          /* the last line of field_seq has been emitted */
    CVBS_EV_LOCK_LOST,          /* no sync: the current field (if any) is void */
} cvbs_event_t;

typedef struct {
    cvbs_event_t type;
    cvbs_std_t   std;
    uint8_t      field;
    uint16_t     lines;         /* active lines per field for std */
    uint32_t     field_seq;
    uint64_t     sample_index;  /* absolute input sample index of the event */
} cvbs_field_event_t;

typedef struct {
    /* Return a job slot to fill (NULL: drop this line). */
    cvbs_line_job_t *(*job_get)(void *ctx);
    /* The filled slot is ready for cvbs_line_decode(). */
    void (*job_put)(void *ctx, cvbs_line_job_t *job);
    void (*event)(void *ctx, const cvbs_field_event_t *ev);
    void *ctx;
} cvbs_sync_sink_t;

typedef struct {
    float    sample_hz;         /* unique input samples per second (20e6) */
    bool     color;             /* allow colour decoding */
    float    saturation;        /* 1.0 = nominal */
    cvbs_std_t force_std;       /* CVBS_STD_NONE = auto */
} cvbs_sync_config_t;

typedef struct {
    bool       locked;
    cvbs_std_t std;
    bool       color;
    float      tip, blank;          /* raw code units */
    float      line_period_us;
    uint32_t   fields;              /* fields started */
    uint32_t   lines_emitted;
    uint32_t   lines_dropped;       /* no job slot available */
    uint32_t   lock_losses;
    uint32_t   missed_syncs;        /* flywheel lines */
    float      burst_ratio;         /* burst amplitude / sync amplitude (0.5 nominal) */
    float      afc_ppm;             /* NCO correction vs nominal fsc/Fs */
} cvbs_sync_stats_t;

typedef struct cvbs_sync cvbs_sync_t;

/* Allocates the sync state (about 20 KiB). */
cvbs_sync_t *cvbs_sync_new(const cvbs_sync_config_t *cfg, const cvbs_sync_sink_t *sink);
void cvbs_sync_free(cvbs_sync_t *s);
/* Feed samples (6-bit codes in the low bits; higher bits are ignored).
 * Emits jobs and events through the sink as lines complete. */
void cvbs_sync_push(cvbs_sync_t *s, const uint8_t *samples, size_t n);
void cvbs_sync_get_stats(const cvbs_sync_t *s, cvbs_sync_stats_t *out);
/* Input samples were lost: drop buffered input and reacquire sync. */
void cvbs_sync_discontinuity(cvbs_sync_t *s);
void cvbs_sync_set_color(cvbs_sync_t *s, bool color, float saturation);

/* Decode one line job into 720 YUYV pixels (CVBS_OUT_STRIDE bytes). */
void cvbs_line_decode(const cvbs_line_job_t *job, uint8_t *yuyv_out);

/* Standard helpers. */
static inline uint16_t cvbs_field_lines(cvbs_std_t std)
{
    return std == CVBS_STD_PAL ? 288 : std == CVBS_STD_NTSC ? 240 : 0;
}
static inline float cvbs_field_rate(cvbs_std_t std)
{
    return std == CVBS_STD_PAL ? 50.0f : 59.94006f;
}

#ifdef __cplusplus
}
#endif

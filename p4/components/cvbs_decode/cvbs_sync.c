/* Composite sync stage: levels, H/V sync, line PLL, standard and field
 * detection, colour burst reference with AFC. See cvbs.h.
 *
 * Conventions (shared with cvbs_line.c and the host test generator):
 *   chroma = U sin(phi) + V cos(phi), phi = carrier phase,
 *   NTSC burst on -U, PAL burst on -U +/- V (+V on lines where V is not
 *   inverted). The NCO runs continuously in input-sample time, so burst
 *   vectors of different lines are directly comparable. A burst vector is
 *   B = sum (x - mean) * (cos ph, sin ph) over the burst window, where ph is
 *   the NCO phase; for x = A cos(ph + t) this gives B ~ (N/2) A e^{-jt}.
 */
#include "cvbs.h"
#include "cvbs_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SYNC_BUF        8192u           /* samples kept for sync analysis */
#define FSC_NTSC        3579545.4545
#define FSC_PAL         4433618.75
#define TWO_PI          6.28318530717958647692f

/* Timing, microseconds from 0H (50 % point of the falling sync edge). */
#define US_TIP0         1.0f
#define US_TIP1         3.5f
#define US_BLANK0       5.2f
#define US_BLANK1       8.6f
#define US_ACT_NTSC     (122.0f / 13.5f)  /* BT.601: first active sample */
#define US_ACT_PAL      (132.0f / 13.5f)
#define US_ACT_LEN      (720.0f / 13.5f)
#define US_RAW_MARGIN   1.6f              /* filter history around the active part */
#define US_BURST0_NTSC  5.5f
#define US_BURST1_NTSC  7.6f
#define US_BURST0_PAL   5.8f
#define US_BURST1_PAL   7.8f

/* Sync pulse classes by width. */
#define US_EQ_MIN       1.0f
#define US_H_MIN        3.5f
#define US_H_MAX        7.0f
#define US_BROAD_MIN    12.0f
#define US_PULSE_CAP    32.0f

#define US_LOCK_WINDOW  1.5f    /* edge search half-window when locked */
#define LOCK_RUN        6       /* consistent h pulses needed to lock */
#define MAX_MISSES      24      /* flywheel lines before unlocking */

/* Lines from the first grid-aligned broad pulse to the first active line. */
#define VSTART_NTSC     18
#define VSTART_PAL      22

enum { PULSE_NONE, PULSE_EQ, PULSE_H, PULSE_BROAD };

struct cvbs_sync {
    cvbs_sync_config_t cfg;
    cvbs_sync_sink_t sink;
    float us;                   /* samples per microsecond */

    uint8_t buf[SYNC_BUF];
    uint64_t base;              /* absolute index of buf[0] */
    size_t len;

    /* Levels. */
    float thr, tip, blank;
    bool levels_ok;
    uint64_t level_block_end;

    /* Acquisition. */
    bool locked;
    uint64_t scan;              /* next sample to scan while unlocked */
    double last_edge;
    double run_period;
    int run;

    /* Line PLL. */
    double next_0h;
    double period;              /* samples per line */
    int misses;

    /* Vertical. */
    cvbs_std_t std;
    int lines_since_vs;         /* grid lines since the last vsync event */
    bool vs_seen;
    bool in_broad;
    uint8_t field;
    uint32_t field_seq;
    bool field_open;            /* FIELD_START sent, FIELD_END not yet */

    /* Subcarrier NCO, continuous in input-sample time. */
    uint32_t inc_nominal;
    uint32_t inc;
    uint64_t ph_ref_idx;
    uint32_t ph_ref;

    /* Burst reference. */
    float b_prev_i, b_prev_q;   /* previous line's burst */
    float b_prev2_i, b_prev2_q; /* two lines back (PAL AFC) */
    float b_avg_i, b_avg_q;     /* reference (-U axis) */
    bool burst_init;
    bool color_on;
    int color_count;
    float burst_ratio;
    uint32_t burst_n;           /* samples in the last burst window */

    cvbs_sync_stats_t stats;
};

/* ---------------------------------------------------------------------- */

static inline uint32_t nco_phase(const cvbs_sync_t *s, uint64_t idx)
{
    /* Modular arithmetic: wraps exactly at 2^32. */
    return s->ph_ref + (uint32_t)(idx - s->ph_ref_idx) * s->inc;
}

static void nco_set_inc(cvbs_sync_t *s, uint32_t inc, uint64_t at)
{
    s->ph_ref = nco_phase(s, at);
    s->ph_ref_idx = at;
    s->inc = inc;
}

static void nco_set_std(cvbs_sync_t *s, cvbs_std_t std, uint64_t at)
{
    const double fsc = std == CVBS_STD_PAL ? FSC_PAL : FSC_NTSC;
    s->inc_nominal = (uint32_t)llround(fsc / s->cfg.sample_hz * 4294967296.0);
    nco_set_inc(s, s->inc_nominal, at);
}

static inline const uint8_t *ptr_at(const cvbs_sync_t *s, uint64_t abs)
{
    return s->buf + (size_t)(abs - s->base);
}

static inline bool have(const cvbs_sync_t *s, double abs_end)
{
    return abs_end < (double)(s->base + s->len) - 2.0;
}

static float mean_range(const cvbs_sync_t *s, double a, double b)
{
    uint64_t i0 = (uint64_t)a, i1 = (uint64_t)b;
    if (i1 <= i0) return 0.0f;
    const uint8_t *p = ptr_at(s, i0);
    uint32_t acc = 0;
    for (uint64_t k = 0; k < i1 - i0; ++k) acc += p[k];
    return (float)acc / (float)(i1 - i0);
}

static float act_us(cvbs_std_t std) { return std == CVBS_STD_PAL ? US_ACT_PAL : US_ACT_NTSC; }

/* Threshold from a histogram while unlocked: the sync tip is the lowest
 * ~7 % of a line; the blanking/black level sits just above it. */
static void bootstrap_levels(cvbs_sync_t *s, uint64_t from, uint64_t to)
{
    uint32_t hist[64] = {0};
    const uint8_t *p = ptr_at(s, from);
    const uint32_t n = (uint32_t)(to - from);
    for (uint32_t k = 0; k < n; ++k) hist[p[k] & 63u]++;
    uint32_t acc = 0;
    int lo = -1, mid = -1;
    for (int c = 0; c < 64; ++c) {
        acc += hist[c];
        if (lo < 0 && acc >= n / 50u) lo = c;           /* 2 % */
        if (mid < 0 && acc >= (n * 15u) / 100u) mid = c;  /* 15 % */
    }
    if (lo < 0 || mid < 0 || mid - lo < 3) { s->levels_ok = false; return; }
    s->tip = (float)lo;
    s->blank = (float)mid;
    s->thr = 0.5f * (s->tip + s->blank);
    s->levels_ok = true;
}

/* Falling crossing of thr at sample i (buf[i-1] >= thr > buf[i]):
 * fractional absolute position of the 50 % point. */
static inline double edge_pos(const cvbs_sync_t *s, uint64_t i)
{
    const float a = ptr_at(s, i - 1)[0], b = ptr_at(s, i)[0];
    const float d = a - b;
    const float f = d > 0.0f ? (a - s->thr) / d : 0.0f;
    return (double)(i - 1) + (double)f;
}

/* Width of the pulse starting at falling-edge sample i, in samples (capped). */
static uint32_t pulse_width(const cvbs_sync_t *s, uint64_t i, uint32_t cap)
{
    const uint8_t *p = ptr_at(s, i);
    const uint64_t avail = s->base + s->len - i;
    if (cap > avail) cap = (uint32_t)avail;
    const int ithr = (int)ceilf(s->thr);
    uint32_t w = 0, hi_run = 0;
    while (w < cap) {
        if (p[w] >= ithr) {
            /* Two samples above threshold end the pulse (noise tolerance). */
            if (++hi_run >= 2) return w - 1;
        } else {
            hi_run = 0;
        }
        ++w;
    }
    return w;
}

static int classify(const cvbs_sync_t *s, uint32_t width)
{
    const float us = (float)width / s->us;
    if (us >= US_BROAD_MIN) return PULSE_BROAD;
    if (us >= US_H_MIN && us <= US_H_MAX) return PULSE_H;
    if (us >= US_EQ_MIN && us < US_H_MIN) return PULSE_EQ;
    return PULSE_NONE;
}

static void emit_event(cvbs_sync_t *s, cvbs_event_t type, uint64_t at)
{
    if (!s->sink.event) return;
    const cvbs_field_event_t ev = {
        .type = type, .std = s->std, .field = s->field,
        .lines = cvbs_field_lines(s->std), .field_seq = s->field_seq,
        .sample_index = at,
    };
    s->sink.event(s->sink.ctx, &ev);
}

static void unlock(cvbs_sync_t *s, uint64_t at)
{
    if (s->field_open) {
        s->field_open = false;
        emit_event(s, CVBS_EV_LOCK_LOST, at);
    }
    s->locked = false;
    s->vs_seen = false;
    s->in_broad = false;
    s->run = 0;
    s->misses = 0;
    s->burst_init = false;
    s->color_on = false;
    s->color_count = 0;
    s->scan = at;
    s->stats.lock_losses++;
}

/* ---------------------------------------------------------------------- */
/* Burst: measure, AFC, reference.                                          */

static void burst_measure(cvbs_sync_t *s, double t0, float *bi, float *bq, float *amp)
{
    const float b0 = s->std == CVBS_STD_PAL ? US_BURST0_PAL : US_BURST0_NTSC;
    const float b1 = s->std == CVBS_STD_PAL ? US_BURST1_PAL : US_BURST1_NTSC;
    const uint64_t i0 = (uint64_t)(t0 + b0 * s->us);
    const uint64_t i1 = (uint64_t)(t0 + b1 * s->us);
    const uint32_t n = (uint32_t)(i1 - i0);
    const uint8_t *p = ptr_at(s, i0);
    float mean = 0.0f;
    for (uint32_t k = 0; k < n; ++k) mean += p[k];
    mean /= (float)n;
    uint32_t ph = nco_phase(s, i0);
    float si = 0.0f, sq = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        const float x = (float)p[k] - mean;
        si += x * cvbs_nco_cos(ph);
        sq += x * cvbs_nco_sin(ph);
        ph += s->inc;
    }
    s->burst_n = n;
    *bi = si;
    *bq = sq;
    *amp = 2.0f * sqrtf(si * si + sq * sq) / (float)n;
}

/* Updates the reference and fills the job's chroma fields. */
static void burst_update(cvbs_sync_t *s, double t0, cvbs_line_job_t *job)
{
    float bi, bq, amp;
    burst_measure(s, t0, &bi, &bq, &amp);
    const float sync_amp = s->blank - s->tip;
    const float ratio = sync_amp > 0.5f ? amp / sync_amp : 0.0f;
    s->burst_ratio += 0.1f * (ratio - s->burst_ratio);

    /* Colour killer with hysteresis (nominal ratio 0.5). */
    if (!s->color_on) {
        if (ratio > 0.2f) { if (++s->color_count >= 10) { s->color_on = true; s->color_count = 0; } }
        else s->color_count = 0;
    } else {
        if (ratio < 0.12f) { if (++s->color_count >= 10) { s->color_on = false; s->color_count = 0; } }
        else s->color_count = 0;
    }

    const bool pal = s->std == CVBS_STD_PAL;
    if (!s->burst_init) {
        s->b_prev_i = s->b_prev2_i = s->b_avg_i = bi;
        s->b_prev_q = s->b_prev2_q = s->b_avg_q = bq;
        s->burst_init = true;
    }

    /* AFC: the burst vector rotates by the NCO frequency error. NTSC: compare
     * with the previous line; PAL: two lines back (same V-switch state). */
    {
        const float ri = pal ? s->b_prev2_i : s->b_prev_i;
        const float rq = pal ? s->b_prev2_q : s->b_prev_q;
        /* arg(B * conj(R)) = -(rotation of the signal phase). */
        const float ci = bi * ri + bq * rq;
        const float cq = bq * ri - bi * rq;
        if (ratio > 0.2f && ci > 0.0f) {
            const float dtheta = -atan2f(cq, ci);       /* signal phase advance */
            const double lines = pal ? 2.0 : 1.0;
            /* Signal advanced by dtheta relative to the NCO in that time:
             * raise the NCO frequency accordingly (gain 0.1). */
            const double dinc = (double)dtheta / (double)TWO_PI / (lines * s->period) * 4294967296.0;
            const uint64_t at = (uint64_t)t0;
            int64_t inc = (int64_t)s->inc + (int64_t)llround(0.1 * dinc);
            const int64_t lim = (int64_t)s->inc_nominal / 2000;   /* +/- 500 ppm */
            if (inc > (int64_t)s->inc_nominal + lim) inc = (int64_t)s->inc_nominal + lim;
            if (inc < (int64_t)s->inc_nominal - lim) inc = (int64_t)s->inc_nominal - lim;
            nco_set_inc(s, (uint32_t)inc, at);
        }
    }

    /* Reference (-U axis). PAL: the mean of consecutive lines cancels the
     * +/-V swing. */
    float ref_i = bi, ref_q = bq;
    if (pal) { ref_i = 0.5f * (bi + s->b_prev_i); ref_q = 0.5f * (bq + s->b_prev_q); }
    s->b_avg_i += 0.25f * (ref_i - s->b_avg_i);
    s->b_avg_q += 0.25f * (ref_q - s->b_avg_q);

    s->b_prev2_i = s->b_prev_i; s->b_prev2_q = s->b_prev_q;
    s->b_prev_i = bi; s->b_prev_q = bq;

    const float mag = sqrtf(s->b_avg_i * s->b_avg_i + s->b_avg_q * s->b_avg_q);
    job->color = s->cfg.color && s->color_on && mag > 0.0f;
    if (!job->color) return;
    /* B ~ e^{-j theta}, theta = burst phase; psi = theta - pi/2 is the U
     * axis: cos psi = sin theta = -Im(Bn), sin psi = -cos theta = -Re(Bn). */
    const float ni = s->b_avg_i / mag, nq = s->b_avg_q / mag;
    job->cos_psi = -nq;
    job->sin_psi = -ni;
    /* Burst amplitude in the same units as the demodulated U/V. The PAL
     * reference is the mean of bursts at +/-45 degrees: |mean| = A cos 45. */
    float bn = 2.0f * mag / (float)(s->burst_n ? s->burst_n : 1u);
    if (pal) bn *= 1.41421356f;
    job->burst_amp = bn;
    if (pal) {
        /* theta_cur - theta_ref = -pi/4 on lines with V not inverted:
         * Im(B * conj(Bavg)) > 0. */
        const float cross = bq * s->b_avg_i - bi * s->b_avg_q;
        job->vsign = cross > 0.0f ? 1.0f : -1.0f;
    } else {
        job->vsign = 1.0f;
    }
}

/* ---------------------------------------------------------------------- */

static void vsync_event(cvbs_sync_t *s, uint64_t at)
{
    const int interval = s->lines_since_vs;
    cvbs_std_t std = s->std;
    uint8_t field = s->field == 1 ? 2 : 1;
    if (interval == 262 || interval == 263) {
        std = CVBS_STD_NTSC;
        field = interval == 263 ? 2 : 1;
    } else if (interval == 312 || interval == 313) {
        std = CVBS_STD_PAL;
        field = interval == 313 ? 2 : 1;
    } else if (std == CVBS_STD_NONE) {
        std = s->period / s->us < 63.78 ? CVBS_STD_NTSC : CVBS_STD_PAL;
    }
    if (s->cfg.force_std != CVBS_STD_NONE) std = s->cfg.force_std;
    if (s->field_open) {
        s->field_open = false;
        emit_event(s, CVBS_EV_FIELD_END, at);
    }
    if (std != s->std) {
        s->std = std;
        nco_set_std(s, std, at);
        s->burst_init = false;
    }
    s->field = field;
    s->field_seq++;
    s->lines_since_vs = 0;
    s->vs_seen = true;
    s->field_open = true;
    s->stats.fields++;
    emit_event(s, CVBS_EV_FIELD_START, at);
}

static void emit_line(cvbs_sync_t *s, double t0, int pulse)
{
    const int vstart = s->std == CVBS_STD_PAL ? VSTART_PAL : VSTART_NTSC;
    const int lines = cvbs_field_lines(s->std);
    const int idx = s->lines_since_vs - vstart;
    if (!s->vs_seen || !s->field_open || idx < 0 || idx >= lines) return;

    cvbs_line_job_t *job = s->sink.job_get ? s->sink.job_get(s->sink.ctx) : NULL;
    if (!job) {
        s->stats.lines_dropped++;
    } else {
        const float a_us = act_us(s->std);
        /* Pixel pitch follows the measured line period (clock mismatch). */
        const double nominal = (s->std == CVBS_STD_PAL ? 64.0 : 63.5555556) * (double)s->us;
        const float scale = (float)(s->period / nominal);
        const float step = s->cfg.sample_hz / 13.5e6f * scale;
        const double raw0 = t0 + (double)((a_us - US_RAW_MARGIN) * s->us * scale);
        const uint64_t r0 = (uint64_t)raw0;
        uint32_t n = (uint32_t)((US_ACT_LEN + 2.0f * US_RAW_MARGIN) * s->us * scale) + 4u;
        if (n > CVBS_LINE_RAW_MAX) n = CVBS_LINE_RAW_MAX;
        memcpy(job->raw, ptr_at(s, r0), n);
        job->n = (uint16_t)n;
        job->std = s->std;
        job->field = s->field;
        job->line = (uint16_t)idx;
        job->field_seq = s->field_seq;
        job->tip = s->tip;
        job->blank = s->blank;
        job->act0 = (float)(t0 + (double)(a_us * s->us * scale) - (double)r0);
        job->px_step = step;
        job->phase0 = nco_phase(s, r0);
        job->inc = s->inc;
        job->saturation = s->cfg.saturation;
        job->color = false;
        job->vsign = 1.0f;
        job->cos_psi = 1.0f;
        job->sin_psi = 0.0f;
        job->burst_amp = 1.0f;
        if (pulse == PULSE_H || pulse == PULSE_NONE) burst_update(s, t0, job);
        s->sink.job_put(s->sink.ctx, job);
        s->stats.lines_emitted++;
    }
    if (idx == lines - 1 && s->field_open) {
        s->field_open = false;
        emit_event(s, CVBS_EV_FIELD_END, (uint64_t)t0);
    }
}

/* One line while locked. Returns false when more input is needed. */
static bool locked_step(cvbs_sync_t *s)
{
    const double pred = s->next_0h;
    const double w = US_LOCK_WINDOW * s->us;
    /* The whole line (and the widest pulse) must be buffered. */
    if (!have(s, pred + s->period + w + 2.0 * s->us)) return false;
    if (pred - w < (double)s->base + 2.0) { unlock(s, s->base + 2); return true; }

    /* Search the falling edge closest to the prediction. */
    const uint64_t i0 = (uint64_t)(pred - w), i1 = (uint64_t)(pred + w);
    const uint8_t *p = ptr_at(s, i0);
    const int ithr = (int)ceilf(s->thr);
    double best = -1.0, best_d = 1e9;
    uint64_t best_i = 0;
    for (uint64_t i = 1; i <= i1 - i0; ++i) {
        if (p[i - 1] >= ithr && p[i] < ithr) {
            const double e = edge_pos(s, i0 + i);
            const double d = fabs(e - pred);
            if (d < best_d) { best_d = d; best = e; best_i = i0 + i; }
        }
    }
    int pulse = PULSE_NONE;
    double t0 = pred;
    if (best >= 0.0) {
        pulse = classify(s, pulse_width(s, best_i, (uint32_t)(US_PULSE_CAP * s->us)));
        if (pulse != PULSE_NONE) {
            const double err = best - pred;
            t0 = pred + 0.6 * err;
            s->period += 0.01 * err;
            const double lo = 62.5 * s->us, hi = 65.0 * s->us;
            if (s->period < lo) s->period = lo;
            if (s->period > hi) s->period = hi;
            s->misses = 0;
        }
    }
    if (pulse == PULSE_NONE) {
        s->stats.missed_syncs++;
        if (++s->misses > MAX_MISSES) { unlock(s, (uint64_t)pred); return true; }
    }

    /* Levels from normal lines. */
    if (pulse == PULSE_H) {
        const float tip = mean_range(s, t0 + US_TIP0 * s->us, t0 + US_TIP1 * s->us);
        const float blank = mean_range(s, t0 + US_BLANK0 * s->us, t0 + US_BLANK1 * s->us);
        if (blank - tip > 2.0f) {
            s->tip += 0.1f * (tip - s->tip);
            s->blank += 0.1f * (blank - s->blank);
            s->thr = 0.5f * (s->tip + s->blank);
        }
    }

    /* Vertical sync: the first grid-aligned broad pulse. */
    if (pulse == PULSE_BROAD) {
        if (!s->in_broad) vsync_event(s, (uint64_t)t0);
        s->in_broad = true;
    } else {
        s->in_broad = false;
        /* Vertical flywheel: a missed vsync is synthesised. */
        if (s->vs_seen) {
            const int max_interval = s->std == CVBS_STD_PAL ? 315 : 265;
            if (s->lines_since_vs > max_interval) vsync_event(s, (uint64_t)t0);
        }
    }

    emit_line(s, t0, pulse);
    s->lines_since_vs++;
    s->next_0h = t0 + s->period;
    return true;
}

/* Unlocked: look for a run of h pulses with a consistent line period. */
static bool search_step(cvbs_sync_t *s)
{
    const uint64_t end = s->base + s->len;
    const uint64_t guard = (uint64_t)(US_PULSE_CAP * s->us) + 4u;
    if (s->scan < s->base + 1) s->scan = s->base + 1;
    if (s->scan + guard >= end) return false;

    if (!s->levels_ok || s->scan >= s->level_block_end) {
        const uint64_t blk = (uint64_t)(70.0f * s->us);
        if (s->scan + blk >= end) return false;
        bootstrap_levels(s, s->scan, s->scan + blk);
        s->level_block_end = s->scan + blk;
        if (!s->levels_ok) { s->scan += blk; return true; }
    }

    const uint64_t stop = end - guard;
    const int ithr = (int)ceilf(s->thr);   /* p >= thr  <=>  p >= ceil(thr) */
    for (uint64_t i = s->scan; i < stop; ++i) {
        const uint8_t *p = ptr_at(s, i);
        if (!(p[-1] >= ithr && p[0] < ithr)) continue;
        const uint32_t wdt = pulse_width(s, i, (uint32_t)(US_PULSE_CAP * s->us));
        const int cls = classify(s, wdt);
        if (cls != PULSE_H) { s->scan = i + (wdt ? wdt : 1); return true; }
        const double e = edge_pos(s, i);
        const double d = e - s->last_edge;
        if (d > 60.0 * s->us && d < 67.0 * s->us) {
            s->run_period = s->run ? s->run_period + 0.25 * (d - s->run_period) : d;
            s->run++;
        } else {
            s->run = 0;
        }
        s->last_edge = e;
        s->scan = i + wdt;
        if (s->run >= LOCK_RUN) {
            s->locked = true;
            s->period = s->run_period;
            s->next_0h = e + s->period;
            s->misses = 0;
            s->lines_since_vs = 0;
            s->vs_seen = false;
            s->in_broad = false;
            if (s->std == CVBS_STD_NONE || s->cfg.force_std != CVBS_STD_NONE) {
                s->std = s->cfg.force_std != CVBS_STD_NONE ? s->cfg.force_std
                       : (s->period / s->us < 63.78 ? CVBS_STD_NTSC : CVBS_STD_PAL);
                nco_set_std(s, s->std, (uint64_t)e);
            }
            /* Seed levels from this pulse. */
            s->tip = mean_range(s, e + US_TIP0 * s->us, e + US_TIP1 * s->us);
            s->blank = mean_range(s, e + US_BLANK0 * s->us, e + US_BLANK1 * s->us);
            if (s->blank - s->tip > 2.0f) s->thr = 0.5f * (s->tip + s->blank);
            return true;
        }
        return true;
    }
    s->scan = stop;
    return false;
}

static void compact(cvbs_sync_t *s)
{
    uint64_t keep = s->locked ? (uint64_t)(s->next_0h - 4.0 * s->us) : s->scan - (uint64_t)(70.0f * s->us);
    if (!s->locked && s->scan < (uint64_t)(70.0f * s->us)) keep = 0;
    if (keep <= s->base) return;
    const uint64_t drop = keep - s->base;
    if (drop >= s->len) { s->base += s->len; s->len = 0; return; }
    memmove(s->buf, s->buf + drop, s->len - (size_t)drop);
    s->len -= (size_t)drop;
    s->base += drop;
}

void cvbs_sync_push(cvbs_sync_t *s, const uint8_t *samples, size_t n)
{
    while (n) {
        if (s->len == SYNC_BUF) {
            compact(s);
            if (s->len == SYNC_BUF) {
                /* Nothing could be released: the state is stuck; restart. */
                unlock(s, s->base + s->len);
                s->base += s->len;
                s->len = 0;
            }
        }
        size_t take = SYNC_BUF - s->len;
        if (take > n) take = n;
        uint8_t *dst = s->buf + s->len;
        for (size_t k = 0; k < take; ++k) dst[k] = samples[k] & 63u;
        s->len += take;
        samples += take;
        n -= take;
        for (;;) {
            const bool progressed = s->locked ? locked_step(s) : search_step(s);
            if (!progressed) break;
        }
        compact(s);
    }
}

cvbs_sync_t *cvbs_sync_new(const cvbs_sync_config_t *cfg, const cvbs_sync_sink_t *sink)
{
    cvbs_tables_init();
    cvbs_sync_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    if (s->cfg.saturation <= 0.0f) s->cfg.saturation = 1.0f;
    s->sink = *sink;
    s->us = cfg->sample_hz / 1e6f;
    s->std = cfg->force_std;
    s->field = 2;
    s->scan = 1;
    nco_set_std(s, s->std == CVBS_STD_NONE ? CVBS_STD_NTSC : s->std, 0);
    return s;
}

void cvbs_sync_free(cvbs_sync_t *s) { free(s); }

void cvbs_sync_discontinuity(cvbs_sync_t *s)
{
    const uint64_t end = s->base + s->len;
    if (s->locked || s->field_open) unlock(s, end);
    s->base = end;
    s->len = 0;
    s->scan = end + 1;
    s->levels_ok = false;
    s->last_edge = 0.0;
}

void cvbs_sync_set_color(cvbs_sync_t *s, bool color, float saturation)
{
    s->cfg.color = color;
    if (saturation > 0.0f) s->cfg.saturation = saturation;
}

void cvbs_sync_get_stats(const cvbs_sync_t *s, cvbs_sync_stats_t *out)
{
    *out = s->stats;
    out->locked = s->locked;
    out->std = s->std;
    out->color = s->color_on;
    out->tip = s->tip;
    out->blank = s->blank;
    out->line_period_us = s->locked ? (float)(s->period / s->us) : 0.0f;
    out->burst_ratio = s->burst_ratio;
    out->afc_ppm = s->inc_nominal ? (float)(((double)s->inc - (double)s->inc_nominal) / (double)s->inc_nominal * 1e6) : 0.0f;
}

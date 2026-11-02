/* Host test for the CVBS decoder: synthesises NTSC/PAL 75 % colour bars as
 * C5VRX would deliver them (6-bit codes, 20 MS/s, small clock offset,
 * noise), decodes them and compares each bar with BT.601 YCbCr.
 *
 *   make -C host   (see host/Makefile)
 *   ./a.out [ntsc|pal] [noise_codes] [out.ppm]
 */
#include "cvbs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS          20e6
#define TIP         4.0
#define BLANK       20.0
#define CHUNK       4032

typedef struct { double y, cb, cr; } ycc_t;

/* 75 % bars: white, yellow, cyan, green, magenta, red, blue, black. */
static const double BAR_RGB[8][3] = {
    {0.75, 0.75, 0.75}, {0.75, 0.75, 0}, {0, 0.75, 0.75}, {0, 0.75, 0},
    {0.75, 0, 0.75}, {0.75, 0, 0}, {0, 0, 0.75}, {0, 0, 0},
};

typedef struct {
    int pal;
    double line_us, fsc, white;
    int lines;                  /* per frame */
    double t;                   /* seconds since start */
    double clock_ppm;           /* receiver sample clock error */
    double noise;
    double sc_phase0;
    unsigned rng;
} gen_t;

static double randn(gen_t *g)
{
    double u1 = ((g->rng = g->rng * 1103515245u + 12345u) >> 8) / 16777216.0 + 1e-12;
    double u2 = ((g->rng = g->rng * 1103515245u + 12345u) >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307 * u2);
}

/* Half-line pulse type: 0 none, 1 equalising, 2 broad. */
static int half_pulse(const gen_t *g, int h)
{
    if (!g->pal) {
        if (h < 6) return 1;
        if (h < 12) return 2;
        if (h < 18) return 1;
        if (h >= 525 && h < 531) return 1;
        if (h >= 531 && h < 537) return 2;
        if (h >= 537 && h < 543) return 1;
        return 0;
    }
    if (h >= 1245) return 1;
    if (h < 5) return 2;
    if (h < 10) return 1;
    if (h >= 620 && h < 625) return 1;
    if (h >= 625 && h < 630) return 2;
    if (h >= 630 && h < 635) return 1;
    return 0;
}

static int line_has_video(const gen_t *g, int line)
{
    if (!g->pal) return (line >= 22 && line <= 262) || (line >= 285 && line <= 525);
    return (line >= 23 && line <= 310) || (line >= 336 && line <= 623);
}

static int line_in_vblank(const gen_t *g, int line)
{
    if (!g->pal) return line <= 9 || (line >= 263 && line <= 272);
    return line <= 5 || line >= 623 || (line >= 311 && line <= 318);
}

/* Smooth sync edge (about 140 ns). */
static double low_pulse(double t_us, double start, double width)
{
    const double rise = 0.14;
    double a = (t_us - start) / rise + 0.5, b = (t_us - start - width) / rise + 0.5;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    b = b < 0 ? 0 : b > 1 ? 1 : b;
    return a - b;   /* 1 inside the pulse */
}

static double sample(gen_t *g, double t)
{
    const double line_s = g->line_us * 1e-6;
    const double frame_s = line_s * g->lines;
    const double tf = fmod(t, frame_s);
    const int line0 = (int)(tf / line_s);            /* 0-based */
    const int line = line0 + 1;
    const double t_us = (tf - line0 * line_s) * 1e6;
    const int h = 2 * line0 + (t_us >= g->line_us / 2 ? 1 : 0);
    const double half_us = g->line_us / 2;
    const double th_us = t_us - (h & 1 ? half_us : 0);
    const int hp = half_pulse(g, h);
    double v = BLANK;
    double sync = 0;
    if (hp == 1) sync = low_pulse(th_us, 0, 2.3);
    else if (hp == 2) sync = low_pulse(th_us, 0, half_us - 4.7);
    else if (!(h & 1) || half_pulse(g, h - 1) == 0) {
        /* Normal line: sync at 0H only (second half of a normal line has none). */
        if (!(h & 1)) sync = low_pulse(t_us, 0, 4.7);
    }
    const double amp = g->white - BLANK;
    const double phi = 6.283185307179586 * g->fsc * t + g->sc_phase0;
    /* PAL V switch: alternate lines (by frame line number). */
    const double vs = g->pal ? ((line & 1) ? 1.0 : -1.0) : 1.0;
    if (!line_in_vblank(g, line) && !(hp)) {
        const double b0 = g->pal ? 5.6 : 5.3;
        const double b1 = b0 + (g->pal ? 10 : 9) / g->fsc * 1e6;
        if (t_us >= b0 && t_us < b1) {
            const double a = 0.5 * (BLANK - TIP);
            if (g->pal) v += a * (-0.70710678 * sin(phi) + vs * 0.70710678 * cos(phi));
            else v += a * -sin(phi);
        }
        const double act0 = g->pal ? 10.5 : 9.4, act1 = g->pal ? 62.5 : 62.2;
        if (line_has_video(g, line) && t_us >= act0 && t_us < act1) {
            /* Bars over the BT.601 window: 720 px from 9.04 / 9.78 us. */
            const double w0 = g->pal ? 132 / 13.5 : 122 / 13.5;
            int bar = (int)((t_us - w0) / (720 / 13.5) * 8);
            if (bar < 0) bar = 0;
            if (bar > 7) bar = 7;
            const double *c = BAR_RGB[bar];
            const double y = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
            const double u = (g->pal ? 0.493 : 0.492) * (c[2] - y);
            const double vv = 0.877 * (c[0] - y);
            v += amp * (y + u * sin(phi) + vs * vv * cos(phi));
        }
    }
    v = v * (1 - sync) + TIP * sync;
    return v;
}

static void gen_fill(gen_t *g, uint8_t *dst, size_t n)
{
    const double dt = 1.0 / (FS * (1.0 + g->clock_ppm * 1e-6));
    for (size_t k = 0; k < n; ++k) {
        double v = sample(g, g->t) + g->noise * randn(g);
        long q = lround(v);
        dst[k] = (uint8_t)(q < 0 ? 0 : q > 63 ? 63 : q);
        g->t += dt;
    }
}

/* ---------------------------------------------------------------------- */

static uint8_t g_field[2][CVBS_MAX_FIELD_LINES * CVBS_OUT_STRIDE];
static int g_cur;
static uint32_t g_cur_seq;
static int g_fields_done;
static uint32_t g_lines_in_field;
static cvbs_line_job_t g_job;
static int g_done_std;

static cvbs_line_job_t *job_get(void *ctx) { (void)ctx; return &g_job; }
static void job_put(void *ctx, cvbs_line_job_t *j)
{
    (void)ctx;
    if (j->field_seq != g_cur_seq) return;
    cvbs_line_decode(j, g_field[g_cur] + (size_t)j->line * CVBS_OUT_STRIDE);
    g_lines_in_field++;
}
static void on_event(void *ctx, const cvbs_field_event_t *ev)
{
    (void)ctx;
    if (ev->type == CVBS_EV_FIELD_START) {
        g_cur ^= 1;
        g_cur_seq = ev->field_seq;
        g_lines_in_field = 0;
        memset(g_field[g_cur], 0, sizeof(g_field[0]));
    } else if (ev->type == CVBS_EV_FIELD_END && ev->field_seq == g_cur_seq) {
        if (g_lines_in_field == ev->lines) { g_fields_done++; g_done_std = ev->std; }
        printf("field %u std=%s parity=%u lines=%u\n", ev->field_seq,
               ev->std == CVBS_STD_PAL ? "PAL" : "NTSC", ev->field, g_lines_in_field);
    } else if (ev->type == CVBS_EV_LOCK_LOST) {
        printf("lock lost\n");
    }
}

static ycc_t bar_mean(const uint8_t *f, int lines, int bar)
{
    ycc_t m = {0};
    int n = 0;
    const int x0 = bar * 90 + 20, x1 = bar * 90 + 70;
    for (int y = lines / 4; y < 3 * lines / 4; ++y) {
        const uint8_t *row = f + (size_t)y * CVBS_OUT_STRIDE;
        for (int x = x0 & ~1; x < x1; x += 2) {
            m.y += row[2 * x] + row[2 * x + 2];
            m.cb += 2.0 * row[2 * x + 1];
            m.cr += 2.0 * row[2 * x + 3];
            n += 2;
        }
    }
    m.y /= n; m.cb /= n; m.cr /= n;
    return m;
}

static void write_ppm(const char *path, const uint8_t *f, int lines)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", CVBS_OUT_WIDTH, lines * 2);
    for (int y = 0; y < lines * 2; ++y) {
        const uint8_t *row = f + (size_t)(y / 2) * CVBS_OUT_STRIDE;
        for (int x = 0; x < CVBS_OUT_WIDTH; ++x) {
            const double Y = row[2 * x] - 16.0;
            const double cb = row[(2 * x & ~3) + 1] - 128.0, cr = row[(2 * x & ~3) + 3] - 128.0;
            double rgb[3] = {1.164 * Y + 1.596 * cr, 1.164 * Y - 0.392 * cb - 0.813 * cr, 1.164 * Y + 2.017 * cb};
            for (int c = 0; c < 3; ++c) fputc(rgb[c] < 0 ? 0 : rgb[c] > 255 ? 255 : (int)rgb[c], fp);
        }
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    const int pal = argc > 1 && !strcmp(argv[1], "pal");
    gen_t g = {
        .pal = pal,
        .line_us = pal ? 64.0 : 63.5555556,
        .fsc = pal ? 4433618.75 : 3579545.4545,
        .white = BLANK + (pal ? 700.0 / 300.0 : 2.5) * (BLANK - TIP),
        .lines = pal ? 625 : 525,
        .t = 0.0123,            /* start somewhere inside a field */
        .clock_ppm = 35.0,
        .noise = argc > 2 ? atof(argv[2]) : 0.4,
        .sc_phase0 = 1.234,
        .rng = 12345,
    };
    const cvbs_sync_config_t cfg = { .sample_hz = (float)FS, .color = true, .saturation = 1.0f };
    const cvbs_sync_sink_t sink = { .job_get = job_get, .job_put = job_put, .event = on_event };
    cvbs_sync_t *s = cvbs_sync_new(&cfg, &sink);
    static uint8_t chunk[CHUNK];
    const int want = 8;
    for (int iter = 0; iter < 200000 && g_fields_done < want; ++iter) {
        gen_fill(&g, chunk, CHUNK);
        cvbs_sync_push(s, chunk, CHUNK);
    }
    cvbs_sync_stats_t st;
    cvbs_sync_get_stats(s, &st);
    printf("locked=%d std=%s color=%d tip=%.2f blank=%.2f period=%.3fus burst_ratio=%.3f afc=%.1fppm "
           "lines=%u dropped=%u missed=%u losses=%u\n",
           st.locked, st.std == CVBS_STD_PAL ? "PAL" : st.std == CVBS_STD_NTSC ? "NTSC" : "none",
           st.color, st.tip, st.blank, st.line_period_us, st.burst_ratio, st.afc_ppm,
           st.lines_emitted, st.lines_dropped, st.missed_syncs, st.lock_losses);
    if (g_fields_done < want) { printf("FAIL: only %d complete fields\n", g_fields_done); return 1; }
    if ((g_done_std == CVBS_STD_PAL) != pal) { printf("FAIL: wrong standard\n"); return 1; }

    /* The last complete field is the one before the current. */
    const uint8_t *f = g_field[g_cur ^ (g_lines_in_field ? 1 : 0)];
    const int lines = cvbs_field_lines(pal ? CVBS_STD_PAL : CVBS_STD_NTSC);
    static const ycc_t want_ycc[8] = {
        {180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
        {84, 184, 198}, {65, 100, 212}, {35, 212, 114}, {16, 128, 128},
    };
    static const char *names[8] = {"white", "yellow", "cyan", "green", "magenta", "red", "blue", "black"};
    double worst = 0;
    for (int b = 0; b < 8; ++b) {
        const ycc_t m = bar_mean(f, lines, b);
        const double e = fmax(fabs(m.y - want_ycc[b].y), fmax(fabs(m.cb - want_ycc[b].cb), fabs(m.cr - want_ycc[b].cr)));
        if (e > worst) worst = e;
        printf("%-8s Y %6.1f (%3.0f)  Cb %6.1f (%3.0f)  Cr %6.1f (%3.0f)  err %.1f\n", names[b],
               m.y, want_ycc[b].y, m.cb, want_ycc[b].cb, m.cr, want_ycc[b].cr, e);
    }
    if (argc > 3) write_ppm(argv[3], f, lines);
    cvbs_sync_free(s);
    /* 6-bit input: one code step is about 5 Y units. */
    const double limit = 12.0;
    printf("%s: worst bar error %.1f (limit %.0f)\n", worst <= limit ? "PASS" : "FAIL", worst, limit);
    return worst <= limit ? 0 : 1;
}

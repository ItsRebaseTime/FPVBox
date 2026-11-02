/* Composite line decoder: one active line -> 720 BT.601 YUYV pixels.
 *
 * All processing runs on the 13.5 MHz output pixel grid:
 *   1. linear resampling of the raw line (line-locked by the sync stage),
 *   2. product demodulation with the continuous subcarrier NCO,
 *   3. two cascaded box filters (chroma low-pass, nulls on the 2*fsc alias),
 *   4. luma = composite - remodulated chroma,
 *   5. U/V rotated into the burst reference, scaled by the burst amplitude.
 * Without colour (no burst, or disabled) a subcarrier notch gives the luma.
 */
#include "cvbs.h"
#include "cvbs_internal.h"
#include <math.h>
#include <stdbool.h>

#define PRE     16
#define POST    16
#define M       (PRE + CVBS_OUT_WIDTH + POST)

/* White - blank over blank - sync tip: 100/40 IRE (NTSC), 700/300 mV (PAL). */
#define K_WHITE_NTSC    2.5f
#define K_WHITE_PAL     (700.0f / 300.0f)
/* Cb/Cr per unit of (U or V) / burst amplitude (derivation in docs/VIDEO.md). */
#define K_U_NTSC        51.40f
#define K_V_NTSC        36.40f
#define K_U_PAL         54.96f
#define K_V_PAL         39.00f

float cvbs_sin_tab[CVBS_SIN_N + CVBS_SIN_N / 4u];

void cvbs_tables_init(void)
{
    static bool done;
    if (done) return;
    for (unsigned k = 0; k < CVBS_SIN_N + CVBS_SIN_N / 4u; ++k)
        cvbs_sin_tab[k] = sinf(6.28318530718f * (float)k / (float)CVBS_SIN_N);
    done = true;
}

static inline uint8_t clamp_u8(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* Box mean over [k - lo, k + hi] for k in [lo, M - hi); zero outside. */
static void box(const float *in, float *out, int lo, int hi)
{
    const int len = lo + hi + 1;
    const float inv = 1.0f / (float)len;
    float acc = 0.0f;
    for (int k = 0; k < len - 1; ++k) acc += in[k];
    for (int k = 0; k < lo; ++k) out[k] = 0.0f;
    for (int k = lo; k < M - hi; ++k) {
        acc += in[k + hi];
        out[k] = acc * inv;
        acc -= in[k - lo];
    }
    for (int k = M - hi; k < M; ++k) out[k] = 0.0f;
}

void cvbs_line_decode(const cvbs_line_job_t *j, uint8_t *out)
{
    float r[M], ps[M], pc[M], t[M], sn[M], cs[M];
    const bool pal = j->std == CVBS_STD_PAL;
    const int nmax = (int)j->n - 2;

    /* 1. Resample to the pixel grid. */
    const float pos0 = j->act0 - (float)PRE * j->px_step;
    float pos = pos0;
    for (int k = 0; k < M; ++k) {
        int i = (int)pos;
        float f = pos - (float)i;
        if (i < 0) { i = 0; f = 0.0f; }
        if (i > nmax) { i = nmax; f = 1.0f; }
        const float a = j->raw[i];
        r[k] = a + f * ((float)j->raw[i + 1] - a);
        pos += j->px_step;
    }

    const float sync_amp = j->blank - j->tip;
    const float g_y = 219.0f / ((sync_amp > 1.0f ? sync_amp : 1.0f) * (pal ? K_WHITE_PAL : K_WHITE_NTSC));
    const float blank = j->blank;

    if (!j->color) {
        /* Subcarrier notch: box over one subcarrier period. */
        if (pal) box(r, t, 1, 1); else box(r, t, 2, 1);
        for (int x = 0; x < CVBS_OUT_WIDTH; x += 2) {
            const int k = PRE + x;
            out[2 * x + 0] = clamp_u8(16.0f + g_y * (t[k] - blank));
            out[2 * x + 1] = 128;
            out[2 * x + 2] = clamp_u8(16.0f + g_y * (t[k + 1] - blank));
            out[2 * x + 3] = 128;
        }
        return;
    }

    /* 2. Demodulate with the NCO, continuous from the sync stage. */
    uint32_t ph = j->phase0 + (uint32_t)(uint64_t)llround((double)pos0 * (double)j->inc);
    const uint32_t inc_px = (uint32_t)llround((double)j->px_step * (double)j->inc);
    for (int k = 0; k < M; ++k) {
        const float s = cvbs_nco_sin(ph), c = cvbs_nco_cos(ph);
        sn[k] = s;
        cs[k] = c;
        ps[k] = r[k] * s;
        pc[k] = r[k] * c;
        ph += inc_px;
    }

    /* 3. Chroma low-pass at 13.5 MHz: NTSC boxes 8 and 4 (centres -0.5/+0.5),
     * PAL boxes 6 and 3; nulls near the 2*fsc alias and the luma above. */
    if (pal) {
        box(ps, t, 3, 2); box(t, ps, 1, 1);
        box(pc, t, 3, 2); box(t, pc, 1, 1);
    } else {
        box(ps, t, 4, 3); box(t, ps, 1, 2);
        box(pc, t, 4, 3); box(t, pc, 1, 2);
    }

    /* 4./5. Luma by remodulation, chroma rotated into the burst frame.
     * 2*LP[x sin] = U cos psi - V sin psi, 2*LP[x cos] = U sin psi + V cos psi. */
    const float amp = j->burst_amp > 0.05f ? j->burst_amp : 0.05f;
    /* The burst amplitude comes from raw samples; the resampled chroma lost
     * the in-phase gain of linear interpolation at the subcarrier, averaged
     * over the fractional positions: 2 (1 - cos w) / w^2 (0.90 NTSC, 0.85 PAL). */
    const float w = (float)j->inc * (6.28318530718f / 4294967296.0f);
    const float interp_gain = 2.0f * (1.0f - cosf(w)) / (w * w);
    const float sat = j->saturation / interp_gain;
    const float g_u = (pal ? K_U_PAL : K_U_NTSC) * sat / amp;
    const float g_v = (pal ? K_V_PAL : K_V_NTSC) * sat / amp * j->vsign;
    const float cp = j->cos_psi, sp = j->sin_psi;
    for (int x = 0; x < CVBS_OUT_WIDTH; x += 2) {
        const int k = PRE + x;
        const float c0 = 2.0f * (ps[k] * sn[k] + pc[k] * cs[k]);
        const float c1 = 2.0f * (ps[k + 1] * sn[k + 1] + pc[k + 1] * cs[k + 1]);
        const float ls = ps[k] + ps[k + 1];     /* 2 x mean of the pair */
        const float lc = pc[k] + pc[k + 1];
        const float u = ls * cp + lc * sp;
        const float v = lc * cp - ls * sp;
        out[2 * x + 0] = clamp_u8(16.0f + g_y * (r[k] - c0 - blank));
        out[2 * x + 1] = clamp_u8(128.0f + g_u * u);
        out[2 * x + 2] = clamp_u8(16.0f + g_y * (r[k + 1] - c1 - blank));
        out[2 * x + 3] = clamp_u8(128.0f + g_v * v);
    }
}

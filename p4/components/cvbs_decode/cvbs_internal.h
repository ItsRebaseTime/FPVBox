/* Shared internals of the CVBS decoder. */
#pragma once
#include <stdint.h>

/* Sine table: 1024 entries per cycle plus a quarter for cosine lookups. */
#define CVBS_SIN_BITS   10
#define CVBS_SIN_N      (1u << CVBS_SIN_BITS)
extern float cvbs_sin_tab[CVBS_SIN_N + CVBS_SIN_N / 4u];
void cvbs_tables_init(void);

static inline float cvbs_nco_sin(uint32_t ph) { return cvbs_sin_tab[ph >> (32 - CVBS_SIN_BITS)]; }
static inline float cvbs_nco_cos(uint32_t ph) { return cvbs_sin_tab[(ph >> (32 - CVBS_SIN_BITS)) + CVBS_SIN_N / 4u]; }

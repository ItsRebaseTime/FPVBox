/* Minimal on-screen text for YUYV buffers (5x7 font, ASCII 32..90). */
#pragma once
#include <stdint.h>

void osd_fill(uint8_t *yuyv, int w, int h, uint8_t y, uint8_t u, uint8_t v);
/* White text; sx/sy scale each font pixel. Lowercase prints as uppercase. */
void osd_text(uint8_t *yuyv, int w, int h, int x, int y, const char *s, int sx, int sy);
void osd_text_centered(uint8_t *yuyv, int w, int h, int y, const char *s, int sx, int sy);

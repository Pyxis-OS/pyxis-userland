#ifndef LIBFONT_STB_ADAPTER_H
#define LIBFONT_STB_ADAPTER_H

#include <font.h>
#include <stddef.h>
#include <stdint.h>

#include "../third_party/stb_truetype/stb_truetype.h"

struct font_backend {
  stbtt_fontinfo info;
};

/* Font bytes stay owned by the face and must outlive this value. */
enum font_result font_backend_init(struct font_backend *backend,
    const uint8_t *bytes, size_t length, uint32_t face_index);
/* The caller checks the raw cmap result against info.numGlyphs before use. */
int font_backend_find_glyph(const struct font_backend *backend, uint32_t scalar);
void font_backend_metrics(const struct font_backend *backend, int *ascent,
    int *descent, int *line_gap);
void font_backend_hmetrics(const struct font_backend *backend, int glyph,
    int *advance, int *bearing);
int font_backend_kerning(const struct font_backend *backend, int left, int right);
int font_backend_glyph_box(const struct font_backend *backend, int glyph,
    int *x0, int *y0, int *x1, int *y1);
float font_backend_scale(const struct font_backend *backend, float pixels);
enum font_result font_backend_bitmap_box(const struct font_backend *backend,
    int glyph, float scale, int *x0, int *y0, int *x1, int *y1);
/* Output is caller-owned. On failure its contents must be discarded. */
enum font_result font_backend_rasterize(const struct font_backend *backend,
    int glyph, float scale, uint8_t *output, int width, int height, int stride,
    int xoff, int yoff, size_t scratch_limit);

#endif

#ifndef PYXIS_FONT_H
#define PYXIS_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Trusted caller-selected font files only. Budgets do not make stb_truetype
 * safe for malicious fonts. Callers serialize each object's use and lifetime. */
typedef int32_t font_fixed; /* Signed 26.6 pixels, including all run coordinates. */

struct font_face;
struct font_size;

enum font_result {
  FONT_OK, FONT_END, FONT_BAD_ARGUMENT, FONT_IO_ERROR, FONT_BAD_FONT,
  FONT_UNSUPPORTED, FONT_NO_MEMORY, FONT_LIMIT
};

struct font_open_options {
  size_t max_file_bytes;
  size_t scratch_bytes;
  uint32_t face_index;
};

struct font_size_options {
  size_t cache_entries;
  size_t cache_bytes;
  size_t scratch_bytes;
  uint32_t max_pixel_height;
};

struct font_metrics {
  font_fixed ascent, descent, line_gap, line_advance;
};

#define FONT_GLYPH_BOX UINT32_MAX

struct font_glyph {
  uint32_t glyph_id; /* FONT_GLYPH_BOX identifies the synthetic replacement. */
  font_fixed advance;
  int32_t left, top; /* Bitmap origin relative to pen: left, -top. */
  uint32_t width, height;
  size_t stride;
  const uint8_t *coverage;
};

struct font_positioned_glyph {
  struct font_glyph glyph;
  int32_t x, y; /* Integer bitmap origin, independent of the successor pen. */
};

struct font_run {
  struct font_size *size;
  const char *text;
  size_t bytes, offset;
  font_fixed pen_x, baseline;
  uint32_t previous_glyph;
  bool has_previous;
};

struct font_xrgb8888 {
  uint32_t *pixels;
  size_t capacity_bytes, stride_bytes;
  uint32_t width, height;
  uint8_t red_shift, green_shift, blue_shift;
};

#ifdef __cplusplus
extern "C" {
#endif

void font_open_options_default(struct font_open_options *options);
void font_size_options_default(struct font_size_options *options);
/* NULL options select defaults. Explicit options must contain nonzero budgets.
 * Open privately owns the file bytes, and closes its FILE on every path.
 * Creation always sets *output to NULL before validation or allocation. */
enum font_result font_open(const char *path, const struct font_open_options *options,
    struct font_face **output);
/* Each size retains its face; it remains usable after font_close(face).
 * Pixels is the ascent-to-descent height, with no DPI or line-gap scaling. */
enum font_result font_size_create(struct font_face *face, uint32_t pixels,
    const struct font_size_options *options, struct font_size **output);
void font_close(struct font_face *face);
void font_size_destroy(struct font_size *size);
void font_cache_clear(struct font_size *size);
enum font_result font_get_metrics(const struct font_size *size, struct font_metrics *output);
/* Coverage is borrowed until the next glyph-producing operation on this size
 * (including failed operations), cache clear or destruction. Copy to retain it.
 * Cache allocations include metadata and empty glyphs. No hidden budget growth. */
enum font_result font_get_glyph(struct font_size *size, uint32_t scalar, struct font_glyph *output);
enum font_result font_get_kerning(const struct font_size *size, uint32_t previous,
    uint32_t next, font_fixed *output);
/* Text and size remain borrowed. Invalid UTF-8 consumes one byte as U+FFFD.
 * Controls are unsupported. On error/END, output is zeroed and run unchanged. */
enum font_result font_run_begin(struct font_size *size, const char *text, size_t bytes,
    font_fixed x, font_fixed baseline, struct font_run *output);
enum font_result font_run_next(struct font_run *run, struct font_positioned_glyph *output);
/* RGB is opaque 0xRRGGBB sRGB. Blend coverage in linear light and preserve all
 * unused destination bits. Invalid descriptors cause no drawing. A later glyph
 * error leaves the rendered prefix and returns its actual error. Text and live
 * library objects must not overlap writable pixel storage. */
enum font_result font_draw_xrgb8888(struct font_size *size, const struct font_xrgb8888 *surface,
    font_fixed x, font_fixed baseline, const char *text, size_t bytes, uint32_t rgb);
const char *font_result_string(enum font_result result);

#ifdef __cplusplus
}
#endif

#endif

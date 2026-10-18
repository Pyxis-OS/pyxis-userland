#include <font.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

union scratch_allocation {
  struct {
    union scratch_allocation *previous;
    union scratch_allocation *next;
    size_t bytes;
  } link;
  max_align_t alignment;
};

struct scratch_context {
  union scratch_allocation *first;
  size_t live;
  size_t limit;
  enum font_result result;
};

static void *scratch_allocate(size_t bytes, void *userdata)
{
  struct scratch_context *context = userdata;
  if (context->result != FONT_OK) {
    return NULL;
  }
  if (bytes == 0) {
    bytes = 1;
  }
  if (bytes > SIZE_MAX - sizeof(union scratch_allocation) ||
      bytes + sizeof(union scratch_allocation) > context->limit - context->live) {
    context->result = FONT_LIMIT;
    return NULL;
  }
  size_t total = bytes + sizeof(union scratch_allocation);
  union scratch_allocation *allocation = malloc(total);
  if (!allocation) {
    context->result = FONT_NO_MEMORY;
    return NULL;
  }
  allocation->link.previous = NULL;
  allocation->link.next = context->first;
  allocation->link.bytes = total;
  if (context->first) {
    context->first->link.previous = allocation;
  }
  context->first = allocation;
  context->live += total;
  return allocation + 1;
}

static void scratch_free(void *pointer, void *userdata)
{
  if (!pointer) {
    return;
  }
  struct scratch_context *context = userdata;
  union scratch_allocation *allocation = (union scratch_allocation *)pointer - 1;
  if (allocation->link.previous) {
    allocation->link.previous->link.next = allocation->link.next;
  } else {
    context->first = allocation->link.next;
  }
  if (allocation->link.next) {
    allocation->link.next->link.previous = allocation->link.previous;
  }
  context->live -= allocation->link.bytes;
  free(allocation);
}

static void scratch_cleanup(struct scratch_context *context)
{
  while (context->first) {
    scratch_free(context->first + 1, context);
  }
}

static void scratch_fail(void *userdata, enum font_result result)
{
  struct scratch_context *context = userdata;
  if (context && context->result == FONT_OK) {
    context->result = result;
  }
}

#define STBTT_malloc(bytes, userdata) scratch_allocate(bytes, userdata)
#define STBTT_free(pointer, userdata) scratch_free(pointer, userdata)
#define STBTT_limit(userdata) scratch_fail(userdata, FONT_LIMIT)
#define STBTT_unsupported(userdata) scratch_fail(userdata, FONT_UNSUPPORTED)
#define STBTT_RASTERIZER_VERSION 2
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_adapter.h"
#pragma GCC diagnostic pop

static int has_bytes(size_t length, size_t offset, size_t count)
{
  return offset <= length && count <= length - offset;
}

static uint16_t read_u16(const uint8_t *bytes)
{
  return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
    (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Recognize the outer SFNT/TTC structure. stb remains a trusted-input parser. */
static enum font_result recognize_font(const uint8_t *bytes, size_t length,
    uint32_t face_index, int *font_offset)
{
  if (length < 12) {
    return FONT_BAD_FONT;
  }
  size_t offset = 0;
  if (memcmp(bytes, "ttcf", 4) == 0) {
    uint32_t version = read_u32(bytes + 4);
    if (version != 0x00010000 && version != 0x00020000) {
      return FONT_UNSUPPORTED;
    }
    uint32_t count = read_u32(bytes + 8);
    if (count > (length - 12) / 4) {
      return FONT_BAD_FONT;
    }
    if (face_index >= count) {
      return FONT_BAD_ARGUMENT;
    }
    offset = read_u32(bytes + 12 + (size_t)face_index * 4);
  } else if (face_index != 0) {
    return FONT_BAD_ARGUMENT;
  }
  if (offset > INT_MAX || !has_bytes(length, offset, 12)) {
    return FONT_BAD_FONT;
  }
  uint32_t signature = read_u32(bytes + offset);
  if (signature != 0x00010000 && signature != 0x31000000 &&
      memcmp(bytes + offset, "OTTO", 4) != 0 &&
      memcmp(bytes + offset, "true", 4) != 0) {
    return FONT_UNSUPPORTED;
  }
  size_t count = read_u16(bytes + offset + 4);
  if (!has_bytes(length, offset + 12, count * 16)) {
    return FONT_BAD_FONT;
  }
  bool has_maxp = false, has_glyf = false, has_cff = false;
  bool unsupported_outlines = false;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t *table = bytes + offset + 12 + i * 16;
    size_t start = read_u32(table + 8);
    size_t size = read_u32(table + 12);
    if (start > INT_MAX || size > INT_MAX - start ||
        !has_bytes(length, start, size)) {
      return FONT_BAD_FONT;
    }
    if (memcmp(table, "glyf", 4) == 0) {
      has_glyf = true;
    } else if (memcmp(table, "CFF ", 4) == 0) {
      has_cff = true;
    } else if (memcmp(table, "CFF2", 4) == 0 || memcmp(table, "SVG ", 4) == 0 ||
        memcmp(table, "sbix", 4) == 0 || memcmp(table, "CBDT", 4) == 0 ||
        memcmp(table, "EBDT", 4) == 0) {
      unsupported_outlines = true;
    }
    size_t minimum = 0;
    if (memcmp(table, "head", 4) == 0) {
      minimum = 54;
    } else if (memcmp(table, "hhea", 4) == 0) {
      minimum = 36;
    } else if (memcmp(table, "hmtx", 4) == 0) {
      minimum = 4;
    } else if (memcmp(table, "maxp", 4) == 0) {
      has_maxp = true;
      minimum = 6;
    } else if (memcmp(table, "cmap", 4) == 0) {
      minimum = 4;
    }
    if (size < minimum) {
      return FONT_BAD_FONT;
    }
    if (memcmp(table, "cmap", 4) == 0) {
      size_t records = read_u16(bytes + start + 2);
      if (records > (size - 4) / 8) {
        return FONT_BAD_FONT;
      }
      for (size_t j = 0; j < records; ++j) {
        size_t subtable = read_u32(bytes + start + 4 + j * 8 + 4);
        if (subtable > size || size - subtable < 2) {
          return FONT_BAD_FONT;
        }
      }
    }
  }
  /* stb otherwise invents a 65535-glyph bound when maxp is absent. */
  if (!has_maxp) {
    return FONT_BAD_FONT;
  }
  if (!has_glyf && !has_cff) {
    return unsupported_outlines ? FONT_UNSUPPORTED : FONT_BAD_FONT;
  }
  *font_offset = (int)offset;
  return FONT_OK;
}

enum font_result font_backend_init(struct font_backend *backend,
    const uint8_t *bytes, size_t length, uint32_t face_index)
{
  if (!backend || !bytes) {
    return FONT_BAD_ARGUMENT;
  }
  memset(backend, 0, sizeof(*backend));
  int offset;
  enum font_result result = recognize_font(bytes, length, face_index, &offset);
  if (result != FONT_OK) {
    return result;
  }
  /* Font initialization only borrows the validated table directory; no scratch. */
  if (!stbtt_InitFont(&backend->info, bytes, offset)) {
    return FONT_BAD_FONT;
  }
  int format = read_u16(bytes + backend->info.index_map);
  if (format != 0 && format != 4 && format != 6 && format != 12 && format != 13) {
    return FONT_UNSUPPORTED;
  }
  if (backend->info.numGlyphs <= 0 || backend->info.indexToLocFormat > 1) {
    return FONT_BAD_FONT;
  }
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&backend->info, &ascent, &descent, &gap);
  if (ascent <= descent) {
    return FONT_BAD_FONT;
  }
  return FONT_OK;
}

int font_backend_find_glyph(const struct font_backend *backend, uint32_t scalar)
{
  return stbtt_FindGlyphIndex(&backend->info, (int)scalar);
}

void font_backend_metrics(const struct font_backend *backend, int *ascent,
    int *descent, int *line_gap)
{
  stbtt_GetFontVMetrics(&backend->info, ascent, descent, line_gap);
}

void font_backend_hmetrics(const struct font_backend *backend, int glyph,
    int *advance, int *bearing)
{
  stbtt_GetGlyphHMetrics(&backend->info, glyph, advance, bearing);
}

int font_backend_kerning(const struct font_backend *backend, int left, int right)
{
  return stbtt_GetGlyphKernAdvance(&backend->info, left, right);
}

int font_backend_glyph_box(const struct font_backend *backend, int glyph,
    int *x0, int *y0, int *x1, int *y1)
{
  *x0 = *y0 = *x1 = *y1 = 0;
  return stbtt_GetGlyphBox(&backend->info, glyph, x0, y0, x1, y1);
}

float font_backend_scale(const struct font_backend *backend, float pixels)
{
  return stbtt_ScaleForPixelHeight(&backend->info, pixels);
}

enum font_result font_backend_bitmap_box(const struct font_backend *backend,
    int glyph, float scale, int *x0, int *y0, int *x1, int *y1)
{
  *x0 = *y0 = *x1 = *y1 = 0;
  if (!(scale > 0) || !isfinite(scale) || glyph < 0 ||
      glyph >= backend->info.numGlyphs) {
    return FONT_BAD_ARGUMENT;
  }
  int gx0, gy0, gx1, gy1;
  if (!font_backend_glyph_box(backend, glyph, &gx0, &gy0, &gx1, &gy1)) {
    return FONT_OK;
  }
  double left = floor((double)gx0 * scale);
  double top = floor(-(double)gy1 * scale);
  double right = ceil((double)gx1 * scale);
  double bottom = ceil(-(double)gy0 * scale);
  if (!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom) ||
      left < INT_MIN || top < INT_MIN || right > INT_MAX || bottom > INT_MAX ||
      right < left || bottom < top || right - left > INT_MAX ||
      bottom - top > INT_MAX) {
    return FONT_LIMIT;
  }
  *x0 = (int)left;
  *y0 = (int)top;
  *x1 = (int)right;
  *y1 = (int)bottom;
  return FONT_OK;
}

enum font_result font_backend_rasterize(const struct font_backend *backend,
    int glyph, float scale, uint8_t *output, int width, int height, int stride,
    int xoff, int yoff, size_t scratch_limit)
{
  if (width < 0 || height < 0 || stride < width || !(scale > 0) ||
      !isfinite(scale) || glyph < 0 || glyph >= backend->info.numGlyphs) {
    return FONT_BAD_ARGUMENT;
  }
  if (width == 0 || height == 0) {
    return FONT_OK;
  }
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  /* stb uses signed int offsets for scanline and scratch arithmetic. */
  if (width > (INT_MAX / (int)sizeof(float) - 1) / 2 ||
      (int64_t)stride * height > INT_MAX ||
      (int64_t)yoff + height >= INT_MAX) {
    return FONT_LIMIT;
  }
  struct scratch_context scratch = {.limit = scratch_limit};
  stbtt_fontinfo info = backend->info;
  info.userdata = &scratch;
  stbtt_vertex *vertices = NULL;
  int count = stbtt_GetGlyphShape(&info, glyph, &vertices);
  enum font_result result = scratch.result;
  if (result == FONT_OK && count == 0) {
    result = FONT_BAD_FONT;
  }
  if (result == FONT_OK) {
    stbtt__bitmap bitmap = {.w = width, .h = height, .stride = stride,
                           .pixels = output};
    stbtt_Rasterize(&bitmap, 0.35f, vertices, count, scale, scale, 0, 0,
                    xoff, yoff, 1, &scratch);
    result = scratch.result;
  }
  scratch_free(vertices, &scratch);
  scratch_cleanup(&scratch);
  return result;
}

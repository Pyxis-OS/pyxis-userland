#include <font.h>
#include "stb_adapter.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct font_face {
  unsigned char *bytes;
  size_t byte_count, references;
  struct font_backend backend;
};

struct font_cache_entry {
  struct font_cache_entry *previous, *next;
  size_t bytes;
  struct font_glyph glyph;
  uint8_t coverage[];
};

struct font_size {
  struct font_face *face;
  struct font_size_options options;
  struct font_metrics metrics;
  uint32_t pixels;
  float scale;
  struct font_cache_entry *first, *last;
  size_t cache_count, cache_bytes;
};

static enum font_result io_result(void)
{
  return errno == ENOMEM ? FONT_NO_MEMORY : FONT_IO_ERROR;
}

void font_open_options_default(struct font_open_options *options)
{
  if (options) {
    *options = (struct font_open_options){
      .max_file_bytes = 32u * 1024u * 1024u,
    };
  }
}

void font_size_options_default(struct font_size_options *options)
{
  if (options) {
    *options = (struct font_size_options){
      .cache_entries = 256, .cache_bytes = 2u * 1024u * 1024u,
      .scratch_bytes = 4u * 1024u * 1024u, .max_pixel_height = 512,
    };
  }
}

enum font_result font_open(const char *path, const struct font_open_options *options,
    struct font_face **output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = NULL;
  struct font_open_options selected;
  font_open_options_default(&selected);
  if (options) {
    selected = *options;
  }
  if (!path || !*path || !selected.max_file_bytes) {
    return FONT_BAD_ARGUMENT;
  }
  errno = 0;
  FILE *file = fopen(path, "rb");
  if (!file) {
    return io_result();
  }
  enum font_result result = FONT_IO_ERROR;
  struct font_face *face = NULL;
  errno = 0;
  if (fseek(file, 0, SEEK_END) != 0) {
    result = io_result();
    goto done;
  }
  errno = 0;
  long length = ftell(file);
  if (length < 0) {
    result = io_result();
    goto done;
  }
  if (!length) {
    result = FONT_BAD_FONT;
    goto done;
  }
  /* stb's table offsets and lengths use signed int internally. */
  if ((uint64_t)length > selected.max_file_bytes || length > INT_MAX) {
    result = FONT_LIMIT;
    goto done;
  }
  errno = 0;
  if (fseek(file, 0, SEEK_SET) != 0) {
    result = io_result();
    goto done;
  }
  face = calloc(1, sizeof(*face));
  if (!face) {
    result = FONT_NO_MEMORY;
    goto done;
  }
  face->byte_count = (size_t)length;
  face->bytes = malloc(face->byte_count);
  if (!face->bytes) {
    result = FONT_NO_MEMORY;
    goto done;
  }
  size_t offset = 0;
  while (offset < face->byte_count) {
    errno = 0;
    size_t amount = fread(face->bytes + offset, 1, face->byte_count - offset, file);
    if (!amount || ferror(file)) {
      result = io_result();
      goto done;
    }
    offset += amount;
  }
  errno = 0;
  if (fseek(file, 0, SEEK_END) != 0) {
    result = io_result();
    goto done;
  }
  errno = 0;
  if (ftell(file) != length) {
    result = io_result();
    goto done;
  }
  result = font_backend_init(&face->backend, face->bytes, face->byte_count,
      selected.face_index);
  if (result == FONT_OK) {
    face->references = 1;
  }

done:
  errno = 0;
  if (fclose(file) != 0 && result == FONT_OK) {
    result = io_result();
  }
  if (result == FONT_OK) {
    *output = face;
  } else if (face) {
    free(face->bytes);
    free(face);
  }
  return result;
}

void font_close(struct font_face *face)
{
  if (face && --face->references == 0) {
    free(face->bytes);
    free(face);
  }
}

static enum font_result scaled_fixed(const struct font_size *size, int units, font_fixed *output)
{
  double value = round((double)units * size->scale * 64.0);
  if (!isfinite(value) || value < INT32_MIN || value > INT32_MAX) {
    return FONT_LIMIT;
  }
  *output = (font_fixed)value;
  return FONT_OK;
}

enum font_result font_size_create(struct font_face *face, uint32_t pixels,
    const struct font_size_options *options, struct font_size **output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = NULL;
  struct font_size_options selected;
  font_size_options_default(&selected);
  if (options) {
    selected = *options;
  }
  if (!face || !pixels || !selected.cache_entries || !selected.cache_bytes ||
      !selected.scratch_bytes || !selected.max_pixel_height) {
    return FONT_BAD_ARGUMENT;
  }
  if (pixels > selected.max_pixel_height || face->references == SIZE_MAX) {
    return FONT_LIMIT;
  }
  struct font_size *size = calloc(1, sizeof(*size));
  if (!size) {
    return FONT_NO_MEMORY;
  }
  size->face = face;
  size->options = selected;
  size->pixels = pixels;
  size->scale = font_backend_scale(&face->backend, (float)pixels);
  int ascent, descent, gap;
  font_backend_metrics(&face->backend, &ascent, &descent, &gap);
  enum font_result result = FONT_BAD_FONT;
  if (ascent > descent && isfinite(size->scale) && size->scale > 0) {
    result = scaled_fixed(size, ascent, &size->metrics.ascent);
    if (result == FONT_OK) {
      result = scaled_fixed(size, descent, &size->metrics.descent);
    }
    if (result == FONT_OK) {
      result = scaled_fixed(size, gap, &size->metrics.line_gap);
    }
    int64_t advance = (int64_t)size->metrics.ascent - size->metrics.descent + size->metrics.line_gap;
    if (result == FONT_OK && (advance < INT32_MIN || advance > INT32_MAX)) {
      result = FONT_LIMIT;
    }
    if (result == FONT_OK) {
      size->metrics.line_advance = (font_fixed)advance;
    }
  }
  if (result != FONT_OK) {
    free(size);
    return result;
  }
  ++face->references;
  *output = size;
  return FONT_OK;
}

static void unlink_entry(struct font_size *size, struct font_cache_entry *entry)
{
  if (entry->previous) {
    entry->previous->next = entry->next;
  } else {
    size->first = entry->next;
  }
  if (entry->next) {
    entry->next->previous = entry->previous;
  } else {
    size->last = entry->previous;
  }
}

static void prepend_entry(struct font_size *size, struct font_cache_entry *entry)
{
  entry->previous = NULL;
  entry->next = size->first;
  if (size->first) {
    size->first->previous = entry;
  } else {
    size->last = entry;
  }
  size->first = entry;
}

static void evict_last(struct font_size *size)
{
  struct font_cache_entry *entry = size->last;
  unlink_entry(size, entry);
  --size->cache_count;
  size->cache_bytes -= entry->bytes;
  free(entry);
}

void font_cache_clear(struct font_size *size)
{
  if (size) {
    while (size->last) {
      evict_last(size);
    }
  }
}

void font_size_destroy(struct font_size *size)
{
  if (size) {
    font_cache_clear(size);
    font_close(size->face);
    free(size);
  }
}

enum font_result font_get_metrics(const struct font_size *size, struct font_metrics *output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = (struct font_metrics){0};
  if (!size) {
    return FONT_BAD_ARGUMENT;
  }
  *output = size->metrics;
  return FONT_OK;
}

static enum font_result cache_glyph(struct font_size *size, uint32_t glyph_id,
    struct font_glyph *output)
{
  for (struct font_cache_entry *entry = size->first; entry; entry = entry->next) {
    if (entry->glyph.glyph_id == glyph_id) {
      unlink_entry(size, entry);
      prepend_entry(size, entry);
      *output = entry->glyph;
      return FONT_OK;
    }
  }
  struct font_glyph glyph = {.glyph_id = glyph_id};
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  enum font_result result = FONT_OK;
  if (glyph_id == FONT_GLYPH_BOX) {
    uint32_t height = (uint64_t)size->pixels * 3 / 4;
    uint32_t width = size->pixels / 2;
    if (height < 3) {
      height = 3;
    }
    if (width < 3) {
      width = 3;
    }
    if (width > INT_MAX || height > INT_MAX || (uint64_t)(width + 1) * 64 > INT32_MAX) {
      return FONT_LIMIT;
    }
    x1 = (int)width;
    y0 = -(int)height;
    glyph.advance = (font_fixed)((width + 1) * 64);
  } else {
    int advance, bearing;
    font_backend_hmetrics(&size->face->backend, (int)glyph_id, &advance, &bearing);
    result = scaled_fixed(size, advance, &glyph.advance);
    if (result == FONT_OK) {
      result = font_backend_bitmap_box(&size->face->backend,
          (int)glyph_id, size->scale, &x0, &y0, &x1, &y1);
    }
    if (result != FONT_OK) {
      return result;
    }
  }
  int64_t width = (int64_t)x1 - x0, height = (int64_t)y1 - y0;
  if (width < 0 || height < 0 || width > INT_MAX || height > INT_MAX || y0 == INT_MIN) {
    return FONT_LIMIT;
  }
  size_t ink_bytes = 0;
  if (width && height) {
    if ((uint64_t)width > SIZE_MAX / (uint64_t)height) {
      return FONT_LIMIT;
    }
    ink_bytes = (size_t)width * (size_t)height;
  }
  if (ink_bytes > SIZE_MAX - sizeof(struct font_cache_entry)) {
    return FONT_LIMIT;
  }
  size_t bytes = sizeof(struct font_cache_entry) + ink_bytes;
  if (bytes > size->options.cache_bytes) {
    return FONT_LIMIT;
  }
  while (size->last && (size->cache_count >= size->options.cache_entries ||
      size->cache_bytes > size->options.cache_bytes - bytes)) {
    evict_last(size);
  }
  struct font_cache_entry *entry = malloc(bytes);
  if (!entry) {
    return FONT_NO_MEMORY;
  }
  memset(entry, 0, sizeof(*entry));
  entry->bytes = bytes;
  glyph.left = x0;
  glyph.top = -y0;
  if (ink_bytes) {
    glyph.width = (uint32_t)width;
    glyph.height = (uint32_t)height;
    glyph.stride = (size_t)width;
    glyph.coverage = entry->coverage;
    memset(entry->coverage, 0, ink_bytes);
    if (glyph_id == FONT_GLYPH_BOX) {
      for (size_t row = 0; row < glyph.height; ++row) {
        for (size_t column = 0; column < glyph.width; ++column) {
          if (!row || row + 1 == glyph.height || !column || column + 1 == glyph.width) {
            entry->coverage[row * glyph.stride + column] = 255;
          }
        }
      }
    } else {
      result = font_backend_rasterize(&size->face->backend, (int)glyph_id, size->scale,
          entry->coverage, (int)width, (int)height, (int)width, x0, y0, size->options.scratch_bytes);
    }
  }
  if (result != FONT_OK) {
    free(entry);
    return result;
  }
  entry->glyph = glyph;
  prepend_entry(size, entry);
  ++size->cache_count;
  size->cache_bytes += bytes;
  *output = glyph;
  return FONT_OK;
}

static bool visible(const struct font_glyph *glyph)
{
  for (size_t row = 0; row < glyph->height; ++row) {
    for (size_t column = 0; column < glyph->width; ++column) {
      if (glyph->coverage[row * glyph->stride + column]) {
        return true;
      }
    }
  }
  return false;
}

static bool control(uint32_t scalar)
{
  return scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f) ||
      scalar == 0x2028 || scalar == 0x2029;
}

enum font_result font_get_glyph(struct font_size *size, uint32_t scalar, struct font_glyph *output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = (struct font_glyph){0};
  if (!size || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
    return FONT_BAD_ARGUMENT;
  }
  if (control(scalar)) {
    return FONT_UNSUPPORTED;
  }
  int glyph = font_backend_find_glyph(&size->face->backend, scalar);
  if (glyph < 0 || glyph >= size->face->backend.info.numGlyphs) {
    return FONT_BAD_FONT;
  }
  if (glyph && scalar != 0xfffd) {
    return cache_glyph(size, (uint32_t)glyph, output);
  }
  if (scalar != 0xfffd) glyph = font_backend_find_glyph(&size->face->backend, 0xfffd);
  if (glyph < 0 || glyph >= size->face->backend.info.numGlyphs) {
    return FONT_BAD_FONT;
  }
  struct font_glyph replacement;
  enum font_result result;
  if (glyph) {
    result = cache_glyph(size, (uint32_t)glyph, &replacement);
    if (result != FONT_OK) {
      return result;
    }
    if (visible(&replacement)) {
      *output = replacement;
      return FONT_OK;
    }
  }
  result = cache_glyph(size, 0, &replacement);
  if (result != FONT_OK) {
    return result;
  }
  if (visible(&replacement)) {
    *output = replacement;
    return FONT_OK;
  }
  return cache_glyph(size, FONT_GLYPH_BOX, output);
}

enum font_result font_get_kerning(const struct font_size *size, uint32_t previous,
    uint32_t next, font_fixed *output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = 0;
  if (!size) {
    return FONT_BAD_ARGUMENT;
  }
  if ((previous != FONT_GLYPH_BOX && previous >= (uint32_t)size->face->backend.info.numGlyphs) ||
      (next != FONT_GLYPH_BOX && next >= (uint32_t)size->face->backend.info.numGlyphs)) {
    return FONT_BAD_ARGUMENT;
  }
  if (previous == FONT_GLYPH_BOX || next == FONT_GLYPH_BOX) {
    return FONT_OK;
  }
  return scaled_fixed(size, font_backend_kerning(&size->face->backend, (int)previous, (int)next), output);
}

static uint32_t decode(const unsigned char *bytes, size_t available, size_t *consumed)
{
  *consumed = 1;
  if (bytes[0] < 0x80) {
    return bytes[0];
  }
  unsigned count;
  uint32_t scalar, minimum;
  if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
    count = 2; scalar = bytes[0] & 0x1f; minimum = 0x80;
  } else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
    count = 3; scalar = bytes[0] & 0x0f; minimum = 0x800;
  } else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
    count = 4; scalar = bytes[0] & 7; minimum = 0x10000;
  } else {
    return 0xfffd;
  }
  if (available < count) {
    return 0xfffd;
  }
  for (unsigned i = 1; i < count; ++i) {
    if ((bytes[i] & 0xc0) != 0x80) {
      return 0xfffd;
    }
    scalar = (scalar << 6) | (bytes[i] & 0x3f);
  }
  if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
    return 0xfffd;
  }
  *consumed = count;
  return scalar;
}

enum font_result font_run_begin(struct font_size *size, const char *text, size_t bytes,
    font_fixed x, font_fixed baseline, struct font_run *output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = (struct font_run){0};
  if (!size || (!text && bytes)) {
    return FONT_BAD_ARGUMENT;
  }
  *output = (struct font_run){.size = size, .text = text, .bytes = bytes, .pen_x = x, .baseline = baseline};
  return FONT_OK;
}

static int64_t floor_fixed(int64_t value)
{
  return value >= 0 ? value / 64 : -((-value + 63) / 64);
}

enum font_result font_run_next(struct font_run *run, struct font_positioned_glyph *output)
{
  if (!output) {
    return FONT_BAD_ARGUMENT;
  }
  *output = (struct font_positioned_glyph){0};
  if (!run || !run->size || (!run->text && run->bytes) || run->offset > run->bytes) {
    return FONT_BAD_ARGUMENT;
  }
  if (run->offset == run->bytes) {
    return FONT_END;
  }
  size_t consumed;
  uint32_t scalar = decode((const unsigned char *)run->text + run->offset, run->bytes - run->offset, &consumed);
  struct font_glyph glyph;
  enum font_result result = font_get_glyph(run->size, scalar, &glyph);
  font_fixed kern = 0;
  if (result == FONT_OK && run->has_previous) {
    result = font_get_kerning(run->size, run->previous_glyph, glyph.glyph_id, &kern);
  }
  if (result != FONT_OK) {
    return result;
  }
  int64_t pen = (int64_t)run->pen_x + kern;
  int64_t next = pen + glyph.advance;
  int64_t x = floor_fixed(pen) + glyph.left;
  int64_t y = floor_fixed(run->baseline) - glyph.top;
  if (pen < INT32_MIN || pen > INT32_MAX || next < INT32_MIN || next > INT32_MAX ||
      x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
    return FONT_LIMIT;
  }
  *output = (struct font_positioned_glyph){.glyph = glyph, .x = (int32_t)x, .y = (int32_t)y};
  run->offset += consumed;
  run->pen_x = (font_fixed)next;
  run->previous_glyph = glyph.glyph_id;
  run->has_previous = true;
  return FONT_OK;
}

const char *font_result_string(enum font_result result)
{
  static const char *const names[] = {
    "ok", "end", "bad argument", "I/O error", "bad font", "unsupported", "no memory", "limit"
  };
  return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown font result";
}

#include <font.h>
#include <math.h>

static bool
channel_shift_valid(uint8_t shift)
{
  return shift <= 24 && shift % 8 == 0;
}

static bool
surface_valid(const struct font_xrgb8888 *surface)
{
  if (!channel_shift_valid(surface->red_shift) ||
      !channel_shift_valid(surface->green_shift) ||
      !channel_shift_valid(surface->blue_shift) ||
      surface->red_shift == surface->green_shift ||
      surface->red_shift == surface->blue_shift ||
      surface->green_shift == surface->blue_shift ||
      surface->stride_bytes % sizeof(uint32_t) != 0) {
    return false;
  }
  if (!surface->width || !surface->height) {
    return true;
  }
  size_t row_bytes;
  if (!surface->pixels ||
      (uintptr_t)surface->pixels % _Alignof(uint32_t) != 0 ||
      __builtin_mul_overflow((size_t)surface->width, sizeof(uint32_t), &row_bytes)) {
    return false;
  }
  if (surface->stride_bytes < row_bytes ||
      surface->height - 1 > (SIZE_MAX - row_bytes) / surface->stride_bytes) {
    return false;
  }
  size_t extent = (size_t)(surface->height - 1) * surface->stride_bytes + row_bytes;
  return extent <= surface->capacity_bytes &&
      extent - 1 <= UINTPTR_MAX - (uintptr_t)surface->pixels;
}

static double
srgb_decode(double value)
{
  return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

static uint32_t
srgb_encode(double value)
{
  double encoded = value <= 0.0031308 ? value * 12.92 :
      1.055 * pow(value, 1.0 / 2.4) - 0.055;
  if (encoded <= 0.0) {
    return 0;
  }
  if (encoded >= 1.0) {
    return 255;
  }
  return (uint32_t)(encoded * 255.0 + 0.5);
}

static void
draw_glyph(const struct font_xrgb8888 *surface,
    const struct font_positioned_glyph *positioned, uint32_t foreground,
    uint32_t channel_mask, const double linear[256], const double color[3])
{
  const struct font_glyph *glyph = &positioned->glyph;
  int64_t left = positioned->x;
  int64_t top = positioned->y;
  int64_t right = left + glyph->width;
  int64_t bottom = top + glyph->height;
  int64_t clipped_left = left < 0 ? 0 : left;
  int64_t clipped_top = top < 0 ? 0 : top;
  int64_t clipped_right = right > surface->width ? surface->width : right;
  int64_t clipped_bottom = bottom > surface->height ? surface->height : bottom;
  if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
    return;
  }

  /* Signed clipping precedes all buffer offsets. The descriptor bounds every
   * destination row, and font_run_next supplies a valid borrowed glyph mask. */
  const uint8_t shifts[3] = { surface->red_shift, surface->green_shift, surface->blue_shift };
  for (int64_t y = clipped_top; y < clipped_bottom; ++y) {
    const uint8_t *coverage = glyph->coverage +
        (size_t)(y - top) * glyph->stride + (size_t)(clipped_left - left);
    uint32_t *destination = (uint32_t *)((uint8_t *)surface->pixels +
        (size_t)y * surface->stride_bytes) + (size_t)clipped_left;
    for (int64_t x = clipped_left; x < clipped_right; ++x, ++coverage, ++destination) {
      if (!*coverage) {
        continue;
      }
      uint32_t background = *destination;
      if (*coverage == 255) {
        *destination = (background & ~channel_mask) | foreground;
        continue;
      }
      double opacity = *coverage / 255.0;
      uint32_t pixel = background & ~channel_mask;
      for (size_t channel = 0; channel < 3; ++channel) {
        uint32_t encoded = (background >> shifts[channel]) & 255;
        double blended = color[channel] * opacity + linear[encoded] * (1.0 - opacity);
        pixel |= srgb_encode(blended) << shifts[channel];
      }
      *destination = pixel;
    }
  }
}

enum font_result
font_draw_xrgb8888(struct font_size *size, const struct font_xrgb8888 *surface,
    font_fixed x, font_fixed baseline, const char *text, size_t bytes, uint32_t rgb)
{
  if (!surface || !surface_valid(surface)) {
    return FONT_BAD_ARGUMENT;
  }
  struct font_xrgb8888 destination = *surface;
  struct font_run run;
  enum font_result result = font_run_begin(size, text, bytes, x, baseline, &run);
  if (result != FONT_OK) {
    return result;
  }

  double linear[256];
  for (size_t value = 0; value < 256; ++value) {
    linear[value] = srgb_decode(value / 255.0);
  }
  double color[3] = { linear[(rgb >> 16) & 255], linear[(rgb >> 8) & 255], linear[rgb & 255] };
  uint32_t foreground = ((rgb >> 16) & 255) << destination.red_shift |
      ((rgb >> 8) & 255) << destination.green_shift |
      (rgb & 255) << destination.blue_shift;
  uint32_t channel_mask = UINT32_C(255) << destination.red_shift |
      UINT32_C(255) << destination.green_shift | UINT32_C(255) << destination.blue_shift;
  struct font_positioned_glyph glyph;
  while ((result = font_run_next(&run, &glyph)) == FONT_OK) {
    draw_glyph(&destination, &glyph, foreground, channel_mask, linear, color);
  }
  return result == FONT_END ? FONT_OK : result;
}

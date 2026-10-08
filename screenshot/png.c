#include "png.h"
#include "../common/directory.h"
#include <file.h>
#include <inttypes.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

#define PNG_RGB_CHANNELS 3
#define PNG_CHANNEL_BITS 8
#define PNG_COMPRESSION_LEVEL 3
#define NATIVE_CHANNEL_MASK UINT32_C(0xff)
#define NATIVE_CHANNEL_SHIFT_MAX 24

struct png_output {
  handle_t file;
  const char *destination;
  uint64_t offset;
  enum call_status native_status;
  png_structp png;
  png_infop info;
  uint32_t *pixels;
  unsigned char *rgb;
  bool complete;
};

static void png_failure(png_structp png, png_const_charp message)
{
  struct png_output *output = png_get_error_ptr(png);
  fprintf(stderr, "screenshot: %s: PNG: %s\n", output->destination, message);
  if (output->native_status != CALL_OK) {
    report_directory_error("screenshot", output->destination, output->native_status);
  }
  png_longjmp(png, 1);
}

static void png_warning_message(png_structp png, png_const_charp message)
{
  const struct png_output *output = png_get_error_ptr(png);
  fprintf(stderr, "screenshot: %s: PNG warning: %s\n", output->destination, message);
}

static void write_png(png_structp png, png_bytep bytes, png_size_t size)
{
  struct png_output *output = png_get_io_ptr(png);
  if (size > UINT64_MAX - output->offset) {
    png_error(png, "Output size exceeds native FILE offsets");
  }
  size_t written = 0;
  while (written < size) {
    size_t count;
    output->native_status = file_write(output->file, output->offset, bytes + written,
        size - written, &count);
    if (output->native_status != CALL_OK) {
      /* Failed writes have no known count, including OUTCOME_UNKNOWN.
       * Unwind immediately; the caller owns the confirmed temporary. */
      png_error(png, "Output FILE write failed");
    }
    if (count == 0 || count > size - written) {
      png_error(png, "Invalid output FILE write progress");
    }
    written += count;
    output->offset += count;
  }
}

static bool snapshot_geometry(const struct screen_capture_reply *snapshot,
    const char *destination, size_t *native_row_bytes, size_t *rgb_row_bytes)
{
  if (snapshot->width == 0 || snapshot->height == 0 ||
      snapshot->width > PNG_UINT_31_MAX || snapshot->height > PNG_UINT_31_MAX) {
    fprintf(stderr, "screenshot: %s: Snapshot dimensions exceed PNG limits\n", destination);
    return false;
  }
  if (snapshot->width > SIZE_MAX / sizeof(uint32_t) ||
      snapshot->width > SIZE_MAX / PNG_RGB_CHANNELS) {
    fprintf(stderr, "screenshot: %s: Snapshot row size is not representable\n", destination);
    return false;
  }
  *native_row_bytes = (size_t)snapshot->width * sizeof(uint32_t);
  *rgb_row_bytes = (size_t)snapshot->width * PNG_RGB_CHANNELS;
  if (snapshot->pitch < *native_row_bytes ||
      snapshot->pitch > UINT64_MAX / snapshot->height ||
      snapshot->size < snapshot->pitch * snapshot->height) {
    fprintf(stderr, "screenshot: %s: Invalid snapshot pitch or FILE extent\n", destination);
    return false;
  }
  if (snapshot->red_shift > NATIVE_CHANNEL_SHIFT_MAX ||
      snapshot->green_shift > NATIVE_CHANNEL_SHIFT_MAX ||
      snapshot->blue_shift > NATIVE_CHANNEL_SHIFT_MAX) {
    fprintf(stderr, "screenshot: %s: Unsupported snapshot channel shifts\n", destination);
    return false;
  }
  uint32_t red = NATIVE_CHANNEL_MASK << snapshot->red_shift;
  uint32_t green = NATIVE_CHANNEL_MASK << snapshot->green_shift;
  uint32_t blue = NATIVE_CHANNEL_MASK << snapshot->blue_shift;
  if ((red & green) || (red & blue) || (green & blue)) {
    fprintf(stderr, "screenshot: %s: Overlapping snapshot color channels\n", destination);
    return false;
  }
  return true;
}

static bool read_row(const struct screen_capture_reply *snapshot, uint64_t row,
    uint32_t *pixels, size_t size, const char *destination)
{
  unsigned char *bytes = (unsigned char *)pixels;
  uint64_t offset = row * snapshot->pitch;
  size_t read = 0;
  while (read < size) {
    size_t count;
    enum call_status status = file_read(snapshot->file, offset + read, bytes + read,
        size - read, &count);
    if (status != CALL_OK) {
      fprintf(stderr, "screenshot: %s: Snapshot FILE read failed at row %" PRIu64 "\n",
          destination, row);
      report_directory_error("screenshot", destination, status);
      return false;
    }
    if (count == 0 || count > size - read) {
      fprintf(stderr, "screenshot: %s: Snapshot FILE ended or returned invalid row data\n",
          destination);
      return false;
    }
    read += count;
  }
  return true;
}

bool screenshot_encode(const struct screen_capture_reply *snapshot, handle_t file,
    const char *destination)
{
  size_t native_row_bytes;
  size_t rgb_row_bytes;
  if (!snapshot_geometry(snapshot, destination, &native_row_bytes, &rgb_row_bytes)) {
    return false;
  }
  /* The pointer stays unchanged across setjmp; all mutable cleanup state lives
   * in the heap object, including libpng allocations made after setjmp. */
  struct png_output *output = calloc(1, sizeof(*output));
  if (!output) {
    fprintf(stderr, "screenshot: %s: Cannot allocate PNG encoder state\n", destination);
    return false;
  }
  output->file = file;
  output->destination = destination;
  output->pixels = malloc(native_row_bytes);
  output->rgb = malloc(rgb_row_bytes);
  if (!output->pixels || !output->rgb) {
    fprintf(stderr, "screenshot: %s: Cannot allocate PNG row buffers\n", destination);
    goto done;
  }
  output->png = png_create_write_struct(PNG_LIBPNG_VER_STRING, output, png_failure,
      png_warning_message);
  if (!output->png) {
    fprintf(stderr, "screenshot: %s: Cannot allocate libpng writer\n", destination);
    goto done;
  }
  jmp_buf *recovery = png_set_longjmp_fn(output->png, longjmp, sizeof(jmp_buf));
  if (!recovery) {
    fprintf(stderr, "screenshot: %s: Cannot allocate libpng error recovery\n", destination);
    goto done;
  }
  if (setjmp(*recovery) != 0) {
    goto done;
  }
  output->info = png_create_info_struct(output->png);
  if (!output->info) {
    fprintf(stderr, "screenshot: %s: Cannot allocate libpng image metadata\n", destination);
    goto done;
  }
  png_set_user_limits(output->png, PNG_UINT_31_MAX, PNG_UINT_31_MAX);
  /* No PNG flush is requested, and pinned libpng does not flush after IEND.
   * Native writes are unbuffered here; this encoder promises no durability. */
  png_set_write_fn(output->png, output, write_png, NULL);
  png_set_IHDR(output->png, output->info, (png_uint_32)snapshot->width,
      (png_uint_32)snapshot->height, PNG_CHANNEL_BITS, PNG_COLOR_TYPE_RGB,
      PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  png_set_compression_level(output->png, PNG_COMPRESSION_LEVEL);
  png_write_info(output->png, output->info);

  for (uint64_t row = 0; row < snapshot->height; ++row) {
    if (!read_row(snapshot, row, output->pixels, native_row_bytes, destination)) {
      goto done;
    }
    for (size_t column = 0; column < snapshot->width; ++column) {
      uint32_t pixel = output->pixels[column];
      output->rgb[column * PNG_RGB_CHANNELS] =
          (unsigned char)(pixel >> snapshot->red_shift);
      output->rgb[column * PNG_RGB_CHANNELS + 1] =
          (unsigned char)(pixel >> snapshot->green_shift);
      output->rgb[column * PNG_RGB_CHANNELS + 2] =
          (unsigned char)(pixel >> snapshot->blue_shift);
    }
    png_write_row(output->png, output->rgb);
  }
  png_write_end(output->png, output->info);
  output->complete = true;

done:
  if (output->png) {
    png_destroy_write_struct(&output->png, &output->info);
  }
  free(output->pixels);
  free(output->rgb);
  bool complete = output->complete;
  free(output);
  return complete;
}

#include <display.h>
#include <startup.h>
#include <stdio.h>
#include <term.h>

#define ITERATION_LIMIT 512

static unsigned escape_iterations(double real, double imaginary)
{
  double x = 0.0;
  double y = 0.0;
  unsigned iteration = 0;

  while (x * x + y * y <= 4.0 && iteration < ITERATION_LIMIT) {
    double next_x = x * x - y * y + real;
    y = 2.0 * x * y + imaginary;
    x = next_x;
    ++iteration;
  }
  return iteration;
}

static uint32_t pixel_color(const struct display_buffer *buffer, uint32_t rgb)
{
  return ((rgb >> 16) & 0xff) << buffer->red_shift |
         ((rgb >> 8) & 0xff) << buffer->green_shift |
         (rgb & 0xff) << buffer->blue_shift;
}

static void render(const struct display_buffer *buffer)
{
  const uint32_t palette[] = {
    0x487fd4, 0x76a8f2, 0x269d9a, 0x52c4c0, 0x52aa60, 0x80d080,
    0xad9b49, 0xc7b461, 0xc26265, 0xe48383, 0xaf5bd1, 0xd58bf0,
    0xb4bcca, 0xdfe5ee,
  };
  uint32_t colors[sizeof(palette) / sizeof(palette[0])];
  for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
    colors[i] = pixel_color(buffer, palette[i]);
  }
  double step = 3.5 / (double)buffer->width;
  double imaginary_top = -step * (double)buffer->height / 2.0;

  for (size_t row = 0; row < buffer->height; ++row) {
    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)
        (buffer->address + row * buffer->pitch);
    double imaginary = imaginary_top + ((double)row + 0.5) * step;

    for (size_t column = 0; column < buffer->width; ++column) {
      double real = -2.5 + ((double)column + 0.5) * step;
      unsigned iteration = escape_iterations(real, imaginary);
      pixels[column] = iteration == ITERATION_LIMIT ? 0 :
          colors[iteration % (sizeof(colors) / sizeof(colors[0]))];
    }
  }
}

int main(void)
{
  handle_t display = startup_resource("display");
  struct terminal term = {
    .input = startup_resource("input"),
    .output = startup_resource("output"),
  };
  if (display == HANDLE_INVALID || term.input == HANDLE_INVALID) {
    fputs("mandelbrot: display and input resources are required\n", stderr);
    return 1;
  }

  struct display_buffer buffer;
  enum call_status status = display_acquire(display, &buffer);
  if (status != CALL_OK) {
    fprintf(stderr, "mandelbrot: cannot acquire display (status %u)\n", (unsigned)status);
    return 1;
  }

  /* Selecting the single buffer first lets periodic presentation show progress.
   * The presenter may read pixels while we write them; tearing is allowed. */
  status = display_present(display);
  if (status == CALL_OK) {
    render(&buffer);
    unsigned key;
    do {
      status = term_read_key(&term, &key);
    } while (status == CALL_INPUT_LOST);
  }

  enum call_status released = display_release(display);
  if (status != CALL_OK || released != CALL_OK) {
    fprintf(stderr, "mandelbrot: display/input failed (status %u, release %u)\n",
        (unsigned)status, (unsigned)released);
    return 1;
  }
  return 0;
}

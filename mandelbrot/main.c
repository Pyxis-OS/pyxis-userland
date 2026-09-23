#include <startup.h>
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

static int render(struct terminal *term, size_t width, size_t height)
{
  const int palette[] = {4, 12, 6, 14, 2, 10, 3, 11, 1, 9, 5, 13, 7, 15};
  const size_t palette_size = sizeof(palette) / sizeof(palette[0]);
  double step = 3.5 / (double)width;
  double imaginary_top = -step * (double)height / 2.0;
  int previous_color = -1;

  for (size_t row = 0; row < height; ++row) {
    if (term_position(term, row, 0) != CALL_OK) {
      return 1;
    }
    double imaginary = imaginary_top + ((double)row + 0.5) * step;

    for (size_t column = 0; column < width; ++column) {
      double real = -2.5 + ((double)column + 0.5) * step;
      unsigned iteration = escape_iterations(real, imaginary);
      int color = iteration == ITERATION_LIMIT ? 0 : palette[iteration % palette_size];
      if (color != previous_color) {
        if (term_colors(term, -1, color) != CALL_OK) {
          return 1;
        }
        previous_color = color;
      }
      /* Two 8x16 terminal cells form one square sample; no UTF-8 needed. */
      if (term_print(term, "  ") != CALL_OK) {
        return 1;
      }
    }
  }
  return 0;
}

int main(void)
{
  struct terminal term = {
    .input = HANDLE_INVALID,
    .output = startup_resource("output"),
  };
  size_t columns, rows;
  if (term.output == HANDLE_INVALID ||
      term_size(&term, &columns, &rows) != CALL_OK || columns < 2 || rows < 2) {
    return 1;
  }

  int result = 1;
  if (term_reset_style(&term) == CALL_OK &&
      term_cursor_visible(&term, false) == CALL_OK && term_clear(&term) == CALL_OK) {
    /* Leave a row for the shell prompt without scrolling the image. */
    result = render(&term, columns / 2, rows - 1);
  }

  if (term_reset_style(&term) != CALL_OK) {
    result = 1;
  }
  if (term_position(&term, rows - 1, 0) != CALL_OK) {
    result = 1;
  }
  if (term_cursor_visible(&term, true) != CALL_OK) {
    result = 1;
  }
  return result;
}

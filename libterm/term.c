#include <term.h>
#include <console.h>
#include <stdio.h>

enum call_status term_read(struct terminal *term, void *bytes, size_t capacity, size_t *read)
{
  return console_read(term->input, bytes, capacity, read);
}

enum call_status term_read_timeout(struct terminal *term, void *bytes, size_t capacity,
                                  uint32_t timeout_ms, size_t *read)
{
  return console_read_timeout(term->input, bytes, capacity, timeout_ms, read);
}

enum call_status term_passthrough(struct terminal *term, handle_t *passthrough)
{
  return console_passthrough(term->input, passthrough);
}

enum call_status term_write(struct terminal *term, const void *bytes, size_t size, size_t *written)
{
  return console_write(term->output, bytes, size, written);
}

enum call_status term_write_all(struct terminal *term, const void *bytes, size_t size)
{
  return console_write_all(term->output, bytes, size);
}

enum call_status term_print(struct terminal *term, const char *text)
{
  return console_print(term->output, text);
}

enum call_status term_fresh_line(struct terminal *term)
{
  return console_fresh_line(term->output);
}

enum call_status term_size(struct terminal *term, size_t *columns, size_t *rows)
{
  if (!columns || !rows) {
    return CALL_BAD_REQUEST;
  }
  *columns = 0;
  *rows = 0;
  struct console_size_reply size;
  enum call_status status = term_geometry(term, &size);
  if (status == CALL_OK) {
    *columns = size.columns;
    *rows = size.rows;
  }
  return status;
}

enum call_status term_geometry(struct terminal *term, struct console_size_reply *size)
{
  return term ? console_size(term->output, size) : CALL_BAD_REQUEST;
}

enum call_status term_set_tab_width(struct terminal *term, size_t columns)
{
  return console_set_tab_width(term->output, columns);
}

static enum call_status move_axis(struct terminal *term, int distance, char positive, char negative)
{
  if (!distance) {
    return CALL_OK;
  }
  char sequence[16];
  int length = snprintf(sequence, sizeof(sequence), "\x1b[%u%c",
      (unsigned)(distance < 0 ? -distance : distance), distance < 0 ? negative : positive);
  return term_write_all(term, sequence, length);
}

enum call_status term_move(struct terminal *term, int rows, int columns)
{
  if (rows < -65535 || rows > 65535 || columns < -65535 || columns > 65535) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = move_axis(term, rows, 'B', 'A');
  return status == CALL_OK ? move_axis(term, columns, 'C', 'D') : status;
}

enum call_status term_position(struct terminal *term, size_t row, size_t column)
{
  if (row >= UINT16_MAX || column >= UINT16_MAX) {
    return CALL_BAD_REQUEST;
  }
  char sequence[24];
  int length = snprintf(sequence, sizeof(sequence), "\x1b[%zu;%zuH", row + 1, column + 1);
  return term_write_all(term, sequence, length);
}

enum call_status term_clear(struct terminal *term)
{
  return term_print(term, "\x1b[2J\x1b[H");
}

enum call_status term_clear_line(struct terminal *term)
{
  return term_print(term, "\x1b[2K");
}

enum call_status term_colors(struct terminal *term, int foreground, int background)
{
  if (foreground < -1 || foreground > 15 || background < -1 || background > 15) {
    return CALL_BAD_REQUEST;
  }
  unsigned fg = foreground < 0 ? 39 : foreground < 8 ? 30 + foreground : 90 + foreground - 8;
  unsigned bg = background < 0 ? 49 : background < 8 ? 40 + background : 100 + background - 8;
  char sequence[24];
  int length = snprintf(sequence, sizeof(sequence), "\x1b[%u;%um", fg, bg);
  return term_write_all(term, sequence, length);
}

enum call_status term_reverse(struct terminal *term, bool enabled)
{
  return term_print(term, enabled ? "\x1b[7m" : "\x1b[27m");
}

enum call_status term_reset_style(struct terminal *term)
{
  return term_print(term, "\x1b[0m");
}

enum call_status term_cursor_visible(struct terminal *term, bool visible)
{
  return term_print(term, visible ? "\x1b[?25h" : "\x1b[?25l");
}

enum call_status term_alternate_screen(struct terminal *term, bool enabled)
{
  return term_print(term, enabled ? "\x1b[?1049h" : "\x1b[?1049l");
}

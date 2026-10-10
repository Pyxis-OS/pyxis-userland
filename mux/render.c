#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SELECTION_FOREGROUND 0
#define SELECTION_BACKGROUND 6

struct output_buffer {
  struct terminal *terminal;
  unsigned char bytes[8192];
  size_t size;
  enum call_status status;
};

static void flush(struct output_buffer *output)
{
  if (output->status == CALL_OK && output->size) {
    output->status = term_write_all(output->terminal, output->bytes, output->size);
  }
  output->size = 0;
}

static void append(struct output_buffer *output, const void *bytes, size_t length)
{
  if (output->status != CALL_OK) {
    return;
  }
  if (length > sizeof(output->bytes) - output->size) {
    flush(output);
  }
  if (output->status == CALL_OK) {
    memcpy(output->bytes + output->size, bytes, length);
    output->size += length;
  }
}

static void position(struct output_buffer *output, size_t row, size_t column)
{
  char bytes[32];
  int length = snprintf(bytes, sizeof(bytes), "\033[%zu;%zuH", row + 1, column + 1);
  append(output, bytes, (size_t)length);
}

static bool same_style(struct terminal_cell a, struct terminal_cell b)
{
  return a.foreground == b.foreground && a.background == b.background &&
      a.attributes == b.attributes;
}

static bool same_cell(struct terminal_cell a, struct terminal_cell b)
{
  return a.glyph == b.glyph && same_style(a, b);
}

static void color_control(char *bytes, size_t capacity, terminal_color color,
    bool foreground)
{
  if (color == TERMINAL_COLOR_DEFAULT) {
    snprintf(bytes, capacity, "%u", foreground ? 39 : 49);
  } else if (color <= 255) {
    snprintf(bytes, capacity, "%u;5;%u", foreground ? 38 : 48, (unsigned)color);
  } else {
    snprintf(bytes, capacity, "%u;2;%u;%u;%u", foreground ? 38 : 48,
        (unsigned)(color >> 16 & 255), (unsigned)(color >> 8 & 255),
        (unsigned)(color & 255));
  }
}

static void style(struct output_buffer *output, struct terminal_cell cell)
{
  char foreground[24], background[24];
  color_control(foreground, sizeof(foreground), cell.foreground, true);
  color_control(background, sizeof(background), cell.background, false);
  /* Reset plus four attributes and two RGB colors uses at most 15 parameters. */
  char bytes[80];
  int length = snprintf(bytes, sizeof(bytes), "\033[0%s%s%s%s;%s;%sm",
      cell.attributes & TERMINAL_ATTR_BOLD ? ";1" : "",
      cell.attributes & TERMINAL_ATTR_ITALIC ? ";3" : "",
      cell.attributes & TERMINAL_ATTR_UNDERLINE ? ";4" : "",
      cell.attributes & TERMINAL_ATTR_REVERSE ? ";7" : "", foreground, background);
  append(output, bytes, (size_t)length);
}

static void label(struct mux *mux, size_t row, size_t column, size_t width,
    const char *text, bool focused)
{
  if (row >= mux->frame_rows) {
    return;
  }
  struct terminal_cell cell = {.glyph = ' ', .foreground = focused ? 0 : 15,
      .background = focused ? 6 : 8};
  for (size_t i = 0; i < width && column + i < mux->frame_columns; ++i) {
    cell.glyph = *text ? (unsigned char)*text++ : ' ';
    mux->frame[row * mux->frame_columns + column + i] = cell;
  }
}

enum call_status mux_render(struct mux *mux)
{
  struct mux_rect view = mux_viewport(mux);
  size_t columns = view.width, rows = view.height;
  if (!columns || !rows) {
    mux->frame_valid = false;
    return CALL_OK;
  }
  if (columns != mux->frame_columns || rows != mux->frame_rows) {
    struct terminal_cell *frame = calloc(columns * rows, sizeof(*frame));
    struct terminal_cell *previous = calloc(columns * rows, sizeof(*previous));
    if (!frame || !previous) {
      free(frame);
      free(previous);
      return CALL_NO_MEMORY;
    }
    free(mux->frame);
    free(mux->previous);
    mux->frame = frame;
    mux->previous = previous;
    mux->frame_columns = columns;
    mux->frame_rows = rows;
    mux->frame_valid = false;
  }
  struct terminal_cell blank = {.glyph = ' ', .foreground = TERMINAL_COLOR_DEFAULT,
      .background = TERMINAL_COLOR_DEFAULT};
  for (size_t i = 0; i < columns * rows; ++i) {
    mux->frame[i] = blank;
  }
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    const struct mux_rect *rect = &mux->rectangles[i];
    struct mux_pane *pane = &mux->panes[i];
    if (!pane->used || !rect->width) {
      continue;
    }
    char title[96];
    if (pane->root_done) {
      snprintf(title, sizeof(title), "[%u%s] %s %lld%s", i + 1, i == mux->focused ? "*" : "",
          pane->result.kind == PROCESS_EXITED ? "exited" :
          pane->result.kind == PROCESS_FAULTED ? "faulted" : "terminated",
          (long long)pane->result.exit_status, pane->group_done ? "" : " (cleaning up)");
    } else {
      snprintf(title, sizeof(title), "[%u%s] shell%s", i + 1, i == mux->focused ? "*" : "",
          pane->remove_when_done ? " (closing)" : pane->browsing ? " (scrollback)" : "");
    }
    label(mux, rect->y, rect->x, rect->width, title, i == mux->focused);
    for (size_t y = 1; y < rect->height; ++y) {
      size_t width;
      const struct terminal_cell *cells = mux_emulator_row(&pane->emulator, pane->scrollback, y - 1, &width);
      if (cells) {
        if (width > rect->width) {
          width = rect->width;
        }
        memcpy(mux->frame + (rect->y + y) * columns + rect->x, cells, width * sizeof(*cells));
        for (size_t x = 0; x < width; ++x) {
          if (mux_emulator_selected(&pane->emulator, pane->scrollback, y - 1, x)) {
            struct terminal_cell *selected = &mux->frame[(rect->y + y) * columns + rect->x + x];
            selected->foreground = SELECTION_FOREGROUND;
            selected->background = SELECTION_BACKGROUND;
            selected->attributes &= (uint8_t)~TERMINAL_ATTR_REVERSE;
          }
        }
      }
    }
    if (rect->x + rect->width < columns) {
      for (size_t y = rect->y; y < rect->y + rect->height; ++y) {
        mux->frame[y * columns + rect->x + rect->width].glyph = '|';
      }
    }
    if (rect->y + rect->height < rows - 1) {
      for (size_t x = rect->x; x < rect->x + rect->width; ++x) {
        mux->frame[(rect->y + rect->height) * columns + x].glyph = '-';
      }
    }
  }
  char footer[256];
  const struct mux_pane *focused = &mux->panes[mux->focused];
  if (mux->confirm) {
    snprintf(footer, sizeof(footer), "Close pane %u and terminate its running work? y/n", mux->focused + 1);
  } else if (focused->browsing) {
    snprintf(footer, sizeof(footer), "Scrollback %zu/%zu | Page Up/Down, Home/End, Esc returns to live",
        focused->scrollback, mux_emulator_history_rows(&focused->emulator));
  } else {
    snprintf(footer, sizeof(footer), "%s%s | Ctrl+B %s %%/\" split, arrows focus, b/e layout, [ history, x close%s",
        mux->layout.kind == MUX_BSP ? "BSP" : "Equal", mux->collapsed ? " (focused only)" : "",
        mux->prefix ? "command:" : "then", mux->notice[0] ? " | " : "");
    size_t used = strlen(footer);
    snprintf(footer + used, sizeof(footer) - used, "%s", mux->notice);
  }
  label(mux, rows - 1, 0, columns, footer, true);

  struct output_buffer output = {.terminal = &mux->terminal};
  append(&output, "\033[?25l", 6);
  if (!mux->frame_valid) {
    append(&output, "\033[0m\033[2J", 8);
  }
  for (size_t y = 0; y < rows; ++y) {
    for (size_t x = 0; x < columns;) {
      size_t index = y * columns + x;
      if (mux->frame_valid && same_cell(mux->frame[index], mux->previous[index])) {
        ++x;
        continue;
      }
      position(&output, y, x);
      struct terminal_cell cell = mux->frame[index];
      style(&output, cell);
      do {
        append(&output, &mux->frame[y * columns + x].glyph, 1);
        ++x;
      } while (x < columns && same_style(cell, mux->frame[y * columns + x]) &&
          (!mux->frame_valid || !same_cell(mux->frame[y * columns + x], mux->previous[y * columns + x])));
    }
  }
  append(&output, "\033[0m", 4);
  const struct mux_rect *rect = &mux->rectangles[mux->focused];
  if (rect->height > 1 && focused->emulator.cursor_visible && !focused->browsing &&
      !mux->confirm && !mux->prefix && !focused->root_done &&
      focused->emulator.cursor_row < rect->height - 1 &&
      focused->emulator.cursor_column < rect->width) {
    position(&output, rect->y + 1 + focused->emulator.cursor_row,
        rect->x + focused->emulator.cursor_column);
    append(&output, "\033[?25h", 6);
  }
  flush(&output);
  if (output.status == CALL_OK) {
    memcpy(mux->previous, mux->frame, rows * columns * sizeof(*mux->frame));
    mux->frame_valid = true;
  }
  return output.status;
}

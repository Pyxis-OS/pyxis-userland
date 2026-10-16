#include "mux.h"
#include <console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SELECTION_FOREGROUND 0
#define SELECTION_BACKGROUND 6

#define MUX_RENDER_SCAN 2048

static void append(struct mux_output *output, const void *bytes, size_t length)
{
  memcpy(output->bytes + output->size, bytes, length);
  output->size += length;
}

static void position(struct mux_output *output, size_t row, size_t column)
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
  return a.character == b.character && same_style(a, b);
}

static void color_control(char *bytes, size_t capacity, terminal_color color,
    bool foreground)
{
  if (color == TERMINAL_COLOR_DEFAULT) {
    snprintf(bytes, capacity, "%u", foreground ? 39 : 49);
  } else if (color < 16) {
    unsigned base = color < 8 ? (foreground ? 30 : 40) : (foreground ? 90 : 100);
    snprintf(bytes, capacity, "%u", base + (unsigned)(color % 8));
  } else if (color <= 255) {
    snprintf(bytes, capacity, "%u;5;%u", foreground ? 38 : 48, (unsigned)color);
  } else {
    snprintf(bytes, capacity, "%u;2;%u;%u;%u", foreground ? 38 : 48,
        (unsigned)(color >> 16 & 255), (unsigned)(color >> 8 & 255),
        (unsigned)(color & 255));
  }
}

static void style(struct mux_output *output, struct terminal_cell cell)
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
  struct terminal_cell cell = {.character = ' ', .foreground = focused ? 0 : 15,
      .background = focused ? 6 : 8};
  for (size_t i = 0; i < width && column + i < mux->frame_columns; ++i) {
    cell.character = *text ? (unsigned char)*text++ : ' ';
    mux->frame[row * mux->frame_columns + column + i] = cell;
  }
}

static enum call_status compose(struct mux *mux)
{
  struct mux_rect view = mux_viewport(mux);
  size_t columns = view.width, rows = view.height;
  if (!columns || !rows) {
    mux->frame_valid = false;
    mux->render_active = false;
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
    mux->render_scan = 0;
    mux->render_again = false;
  }
  struct terminal_cell blank = {.character = ' ', .foreground = TERMINAL_COLOR_DEFAULT,
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
        mux->frame[y * columns + rect->x + rect->width].character = '|';
      }
    }
    if (rect->y + rect->height < rows - 1) {
      for (size_t x = rect->x; x < rect->x + rect->width; ++x) {
        mux->frame[(rect->y + rect->height) * columns + x].character = '-';
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
    snprintf(footer, sizeof(footer), "%s%s | Ctrl+B %s %%/\" split, digits/arrows focus, b/e layout, z zoom, [ history, x close%s",
        mux->layout.kind == MUX_BSP ? "BSP" : "Equal",
        mux->zoomed ? " (zoomed)" : mux->collapsed ? " (focused only)" : "",
        mux->prefix ? "command:" : "then", mux->notice[0] ? " | " : "");
    size_t used = strlen(footer);
    snprintf(footer + used, sizeof(footer) - used, "%s", mux->notice);
  }
  label(mux, rows - 1, 0, columns, footer, true);

  return CALL_OK;
}

static void cursor(struct mux *mux)
{
  struct mux_output *output = &mux->output;
  append(output, "\033[?25l\033[0m", 10);
  const struct mux_pane *focused = &mux->panes[mux->focused];
  const struct mux_rect *rect = &mux->rectangles[mux->focused];
  if (rect->height > 1 && focused->emulator.cursor_visible && !focused->browsing &&
      !mux->confirm && !mux->prefix && !focused->root_done &&
      focused->emulator.cursor_row < rect->height - 1 &&
      focused->emulator.cursor_column < rect->width) {
    position(output, rect->y + 1 + focused->emulator.cursor_row,
        rect->x + focused->emulator.cursor_column);
    append(output, "\033[?25h", 6);
  }
  output->finish = true;
}

static enum call_status flush(struct mux *mux, size_t *budget, bool *progress)
{
  struct mux_output *output = &mux->output;
  size_t length = output->size - output->at;
  if (length > *budget) {
    length = *budget;
  }
  size_t written;
  enum call_status status = console_try_write(mux->terminal.output,
      output->bytes + output->at, length, &written);
  output->blocked = status == CALL_WOULD_BLOCK;
  if (status == CALL_WOULD_BLOCK) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  output->at += written;
  *budget -= written;
  *progress = true;
  /* Each snapshot becomes the accepted cell only after its entire UTF-8
   * glyph, including any partial suffix, has entered the outer queue. */
  while (output->committed < output->count &&
      output->at >= output->ends[output->committed]) {
    mux->previous[output->first + output->committed] = output->cells[output->committed];
    ++output->committed;
  }
  if (output->at != output->size) {
    return CALL_OK;
  }
  if (output->reset) {
    for (size_t i = 0; i < mux->frame_columns * mux->frame_rows; ++i) {
      mux->previous[i].character = UINT16_MAX;
    }
    mux->frame_valid = true;
  }
  if (output->finish) {
    mux->render_active = false;
    mux->render_scan = 0;
    if (!mux->dirty && mux_pointer_view_pending(mux)) {
      status = mux_pointer_advance_view(mux);
      if (status == CALL_OK) {
        mux->rendered_pointer_mapping = mux->pointer_geometry.surface.mapping_identity;
      }
    }
  }
  output->size = output->at = 0;
  output->count = output->committed = 0;
  output->reset = output->finish = output->blocked = false;
  return status;
}

enum call_status mux_render(struct mux *mux, bool *progress)
{
  size_t budget = MUX_RENDER_BYTES, scanned = 0;
  while (budget && scanned < MUX_RENDER_SCAN) {
    if (mux->output.size) {
      enum call_status status = flush(mux, &budget, progress);
      if (status != CALL_OK || mux->output.size) {
        return status;
      }
    }
    /* Finish queued bytes before reallocating: they name cells and control
     * sequences from the old geometry. Dirty state coalesces while stalled. */
    if (mux->dirty) {
      bool again = mux->render_active && mux->render_scan;
      enum call_status status = compose(mux);
      if (status != CALL_OK) {
        return status;
      }
      mux->dirty = false;
      if (!mux->frame_columns || !mux->frame_rows ||
          !mux_viewport(mux).width || !mux_viewport(mux).height) {
        return CALL_OK;
      }
      mux->render_active = true;
      mux->render_again |= again;
      *progress = true;
    }
    if (!mux->render_active) {
      return CALL_OK;
    }
    struct mux_output *output = &mux->output;
    if (!mux->frame_valid) {
      append(output, "\033[?25l\033[0m\033[2J", 14);
      output->reset = true;
      continue;
    }
    size_t total = mux->frame_columns * mux->frame_rows;
    while (mux->render_scan < total && scanned < MUX_RENDER_SCAN &&
        same_cell(mux->frame[mux->render_scan], mux->previous[mux->render_scan])) {
      ++mux->render_scan;
      ++scanned;
      *progress = true;
    }
    if (mux->render_scan == total) {
      if (mux->render_again) {
        mux->render_scan = 0;
        mux->render_again = false;
        continue;
      }
      cursor(mux);
      continue;
    }
    if (scanned == MUX_RENDER_SCAN) {
      break;
    }
    size_t first = mux->render_scan;
    size_t row = first / mux->frame_columns;
    size_t column = first % mux->frame_columns;
    append(output, "\033[?25l", 6);
    position(output, row, column);
    struct terminal_cell cell = mux->frame[first];
    style(output, cell);
    output->first = first;
    do {
      char bytes[3];
      struct terminal_cell next = mux->frame[mux->render_scan];
      size_t length = terminal_character_encode(next.character, bytes);
      append(output, bytes, length);
      output->cells[output->count] = next;
      output->ends[output->count++] = output->size;
      ++mux->render_scan;
      ++column;
      ++scanned;
    } while (column < mux->frame_columns && output->count < MUX_RENDER_CELLS &&
        scanned < MUX_RENDER_SCAN && same_style(cell, mux->frame[mux->render_scan]) &&
        !same_cell(mux->frame[mux->render_scan], mux->previous[mux->render_scan]));
  }
  return CALL_OK;
}

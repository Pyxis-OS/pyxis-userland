#include "emulator.h"

#include <abi/terminal.h>
#include <stdlib.h>
#include <string.h>

static bool valid_geometry(size_t columns, size_t rows)
{
  return columns && columns <= TERMINAL_COLUMNS_MAX &&
      rows && rows <= TERMINAL_ROWS_MAX;
}

static struct mux_cell blank_cell(const struct mux_emulator *emulator,
    bool reverse)
{
  return (struct mux_cell) {
    .character = ' ',
    .foreground = emulator->foreground,
    .background = emulator->background,
    .reverse = reverse,
  };
}

static void fill_cells(struct mux_cell *cells, size_t count, struct mux_cell cell)
{
  for (size_t i = 0; i < count; ++i) {
    cells[i] = cell;
  }
}

void mux_emulator_clear_selection(struct mux_emulator *emulator)
{
  emulator->selection = (struct mux_selection){0};
}

static const struct mux_cell *selected_row(const struct mux_emulator *emulator,
    uint64_t row, size_t *width)
{
  uint64_t oldest = emulator->scrolled_rows - emulator->history_count;
  if (row < oldest) {
    return NULL;
  }
  if (row < emulator->scrolled_rows) {
    size_t slot = (emulator->history_start + (size_t)(row - oldest)) % MUX_HISTORY_ROWS;
    *width = emulator->history_widths[slot];
    return emulator->history + slot * emulator->history_stride;
  }
  uint64_t live = row - emulator->scrolled_rows;
  if (live >= emulator->rows) {
    return NULL;
  }
  *width = emulator->columns;
  return emulator->cells + (size_t)live * emulator->columns;
}

enum call_status mux_emulator_copy_selection(const struct mux_emulator *emulator,
    size_t limit, char **text, size_t *length)
{
  *text = NULL;
  *length = 0;
  if (!emulator->selection.active || emulator->selection.pending) {
    return CALL_NOT_FOUND;
  }
  struct mux_selection_point first = emulator->selection.anchor;
  struct mux_selection_point last = emulator->selection.end;
  if (last.row < first.row || (last.row == first.row && last.column < first.column)) {
    struct mux_selection_point swap = first;
    first = last;
    last = swap;
  }
  if (last.row - first.row > limit) {
    return CALL_LIMIT;
  }
  /* The mux loop owns these cells; neither pass yields to feed or resize. */
  char *owned = NULL;
  size_t total = 0;
  for (unsigned pass = 0; pass < 2; ++pass) {
    size_t at = 0;
    for (uint64_t row = first.row;; ++row) {
      size_t width;
      const struct mux_cell *cells = selected_row(emulator, row, &width);
      if (!cells) {
        free(owned);
        return CALL_UNAVAILABLE;
      }
      if (width > emulator->selection.columns) {
        width = emulator->selection.columns;
      }
      size_t begin = row == first.row ? first.column : 0;
      size_t end = row == last.row && last.column < width ? last.column + 1 : width;
      if (begin > end || (row == first.row && begin >= width) ||
          (row == last.row && last.column >= width)) {
        free(owned);
        return CALL_UNAVAILABLE;
      }
      for (size_t column = begin; column < end; ++column) {
        if (cells[column].character < ' ' || cells[column].character > '~') {
          free(owned);
          return CALL_BAD_REQUEST;
        }
      }
      while (end > begin && cells[end - 1].character == ' ') {
        --end;
      }
      size_t count = end - begin;
      size_t separator = row != last.row;
      if (count > limit - at || separator > limit - at - count) {
        free(owned);
        return CALL_LIMIT;
      }
      if (pass) {
        for (size_t column = begin; column < end; ++column) {
          owned[at++] = cells[column].character;
        }
        if (separator) {
          owned[at++] = '\n';
        }
      } else {
        at += count + separator;
      }
      if (row == last.row) {
        break;
      }
    }
    if (!pass) {
      total = at;
      owned = malloc(total ? total : 1);
      if (!owned) {
        return CALL_NO_MEMORY;
      }
    }
  }
  *text = owned;
  *length = total;
  return CALL_OK;
}

static bool point_before(struct mux_selection_point a, struct mux_selection_point b)
{
  return a.row < b.row || (a.row == b.row && a.column < b.column);
}

static bool selection_contains(const struct mux_emulator *emulator,
    struct mux_selection_point point)
{
  if (point.column >= emulator->selection.columns) {
    return false;
  }
  struct mux_selection_point first = emulator->selection.anchor;
  struct mux_selection_point last = emulator->selection.end;
  if (point_before(last, first)) {
    struct mux_selection_point swap = first;
    first = last;
    last = swap;
  }
  return !point_before(point, first) && !point_before(last, point);
}

static void write_cell(struct mux_emulator *emulator, size_t index, struct mux_cell cell)
{
  struct mux_selection_point point = {
    .row = emulator->scrolled_rows + index / emulator->columns,
    .column = index % emulator->columns,
  };
  if (emulator->cells[index].character != cell.character &&
      (emulator->selection.active || emulator->selection.pending) &&
      selection_contains(emulator, point)) {
    mux_emulator_clear_selection(emulator);
  }
  emulator->cells[index] = cell;
}

bool mux_emulator_init(struct mux_emulator *emulator, size_t columns, size_t rows)
{
  if (!valid_geometry(columns, rows)) {
    return false;
  }

  struct mux_emulator initial = {
    .columns = columns,
    .rows = rows,
    .cursor_visible = true,
    .foreground = -1,
    .background = -1,
    .tab_width = 8,
    .history_stride = columns,
  };
  initial.cells = calloc(rows * columns, sizeof(*initial.cells));
  initial.history = calloc(MUX_HISTORY_ROWS * columns, sizeof(*initial.history));
  initial.history_widths = calloc(MUX_HISTORY_ROWS, sizeof(*initial.history_widths));
  if (!initial.cells || !initial.history || !initial.history_widths) {
    mux_emulator_destroy(&initial);
    return false;
  }
  fill_cells(initial.cells, rows * columns, blank_cell(&initial, false));
  *emulator = initial;
  return true;
}

void mux_emulator_destroy(struct mux_emulator *emulator)
{
  free(emulator->cells);
  free(emulator->history);
  free(emulator->history_widths);
  memset(emulator, 0, sizeof(*emulator));
}

bool mux_emulator_resize(struct mux_emulator *emulator, size_t columns, size_t rows)
{
  if (!valid_geometry(columns, rows)) {
    return false;
  }
  if (columns == emulator->columns && rows == emulator->rows) {
    return true;
  }

  struct mux_cell *cells = calloc(rows * columns, sizeof(*cells));
  if (!cells) {
    return false;
  }
  struct mux_cell *history = emulator->history;
  if (columns > emulator->history_stride) {
    history = calloc(MUX_HISTORY_ROWS * columns, sizeof(*history));
    if (!history) {
      free(cells);
      return false;
    }
    for (size_t i = 0; i < emulator->history_count; ++i) {
      size_t slot = (emulator->history_start + i) % MUX_HISTORY_ROWS;
      memcpy(history + slot * columns,
          emulator->history + slot * emulator->history_stride,
          emulator->history_widths[slot] * sizeof(*history));
    }
  }

  fill_cells(cells, rows * columns, blank_cell(emulator, false));
  size_t first_row = emulator->cursor_row >= rows ?
      emulator->cursor_row - rows + 1 : 0;
  size_t retained_rows = emulator->rows - first_row;
  if (retained_rows > rows) {
    retained_rows = rows;
  }
  size_t retained_columns = emulator->columns < columns ? emulator->columns : columns;
  for (size_t i = 0; i < retained_rows; ++i) {
    memcpy(cells + i * columns,
        emulator->cells + (first_row + i) * emulator->columns,
        retained_columns * sizeof(*cells));
  }

  free(emulator->cells);
  emulator->cells = cells;
  if (history != emulator->history) {
    free(emulator->history);
    emulator->history = history;
    emulator->history_stride = columns;
  }
  emulator->columns = columns;
  emulator->rows = rows;
  if (emulator->cursor_column >= columns) {
    emulator->cursor_column = columns - 1;
  }
  emulator->cursor_row -= first_row;
  emulator->wrap_pending = false;
  mux_emulator_clear_selection(emulator);
  return true;
}

const struct mux_cell *mux_emulator_row(const struct mux_emulator *emulator,
    size_t scrollback_offset, size_t visible_row, size_t *width)
{
  *width = 0;
  if (visible_row >= emulator->rows) {
    return NULL;
  }
  if (scrollback_offset > emulator->history_count) {
    scrollback_offset = emulator->history_count;
  }
  size_t row = emulator->history_count - scrollback_offset + visible_row;
  if (row < emulator->history_count) {
    size_t slot = (emulator->history_start + row) % MUX_HISTORY_ROWS;
    *width = emulator->history_widths[slot];
    return emulator->history + slot * emulator->history_stride;
  }
  *width = emulator->columns;
  return emulator->cells + (row - emulator->history_count) * emulator->columns;
}

bool mux_emulator_select(struct mux_emulator *emulator, size_t scrollback_offset,
    size_t row, size_t column, size_t visible_columns, bool extend, bool activate)
{
  size_t width;
  if (!mux_emulator_row(emulator, scrollback_offset, row, &width) || column >= width ||
      column >= visible_columns ||
      emulator->scrolled_rows > UINT64_MAX - emulator->rows) {
    return false;
  }
  if (scrollback_offset > emulator->history_count) {
    scrollback_offset = emulator->history_count;
  }
  struct mux_selection_point point = {
    .row = emulator->scrolled_rows - scrollback_offset + row,
    .column = column,
  };
  if (!extend) {
    emulator->selection.anchor = point;
    emulator->selection.columns = visible_columns;
    emulator->selection.active = false;
    emulator->selection.pending = true;
  } else if (!emulator->selection.active && !emulator->selection.pending) {
    return false;
  }
  if (extend && activate) {
    emulator->selection.active = true;
    emulator->selection.pending = false;
  }
  emulator->selection.end = point;
  return true;
}

bool mux_emulator_selected(const struct mux_emulator *emulator,
    size_t scrollback_offset, size_t row, size_t column)
{
  if (scrollback_offset > emulator->history_count) {
    scrollback_offset = emulator->history_count;
  }
  return emulator->selection.active && selection_contains(emulator, (struct mux_selection_point){
    .row = emulator->scrolled_rows - scrollback_offset + row,
    .column = column,
  });
}

static void newline(struct mux_emulator *emulator)
{
  emulator->wrap_pending = false;
  emulator->cursor_column = 0;
  ++emulator->cursor_row;
  if (emulator->cursor_row < emulator->rows) {
    return;
  }

  size_t slot;
  if (emulator->history_count == MUX_HISTORY_ROWS) {
    slot = emulator->history_start;
    emulator->history_start = (emulator->history_start + 1) % MUX_HISTORY_ROWS;
  } else {
    slot = (emulator->history_start + emulator->history_count) % MUX_HISTORY_ROWS;
    ++emulator->history_count;
  }
  memcpy(emulator->history + slot * emulator->history_stride, emulator->cells,
      emulator->columns * sizeof(*emulator->cells));
  emulator->history_widths[slot] = emulator->columns;
  if (emulator->scrolled_rows < UINT64_MAX) {
    ++emulator->scrolled_rows;
  }
  uint64_t oldest = emulator->scrolled_rows - emulator->history_count;
  if (emulator->scrolled_rows > UINT64_MAX - emulator->rows ||
      ((emulator->selection.active || emulator->selection.pending) &&
       (emulator->selection.anchor.row < oldest || emulator->selection.end.row < oldest))) {
    mux_emulator_clear_selection(emulator);
  }
  memmove(emulator->cells, emulator->cells + emulator->columns,
      (emulator->rows - 1) * emulator->columns * sizeof(*emulator->cells));
  emulator->cursor_row = emulator->rows - 1;
  /* Native scrolling fills the new row with non-reversed current colors. */
  fill_cells(emulator->cells + emulator->cursor_row * emulator->columns,
      emulator->columns, blank_cell(emulator, false));
}

void mux_emulator_fresh_line(struct mux_emulator *emulator)
{
  emulator->escape_state = MUX_TEXT;
  if (emulator->cursor_column || emulator->wrap_pending) {
    newline(emulator);
  }
}

void mux_emulator_set_tab_width(struct mux_emulator *emulator, unsigned columns)
{
  if (columns >= 1 && columns <= 32) {
    emulator->tab_width = columns;
  }
}

static void erase_cells(struct mux_emulator *emulator, size_t first, size_t end)
{
  struct mux_cell blank = blank_cell(emulator, emulator->reverse);
  for (size_t i = first; i < end; ++i) {
    write_cell(emulator, i, blank);
  }
}

static void select_style(struct mux_emulator *emulator, unsigned parameter)
{
  if (parameter == 0) {
    emulator->foreground = -1;
    emulator->background = -1;
    emulator->reverse = false;
  } else if (parameter == 7 || parameter == 27) {
    emulator->reverse = parameter == 7;
  } else if (parameter == 39) {
    emulator->foreground = -1;
  } else if (parameter == 49) {
    emulator->background = -1;
  } else if (parameter >= 30 && parameter <= 37) {
    emulator->foreground = parameter - 30;
  } else if (parameter >= 40 && parameter <= 47) {
    emulator->background = parameter - 40;
  } else if (parameter >= 90 && parameter <= 97) {
    emulator->foreground = parameter - 90 + 8;
  } else if (parameter >= 100 && parameter <= 107) {
    emulator->background = parameter - 100 + 8;
  }
}

static void execute_csi(struct mux_emulator *emulator, unsigned char command)
{
  unsigned parameter = emulator->parameters[0];
  size_t count = parameter ? parameter : 1;
  size_t cell = emulator->cursor_row * emulator->columns + emulator->cursor_column;
  size_t cells = emulator->rows * emulator->columns;

  if (emulator->private_csi) {
    if (emulator->parameter_index == 0 && parameter == 25 &&
        (command == 'h' || command == 'l')) {
      emulator->cursor_visible = command == 'h';
    }
    return;
  }
  if (command == 'm') {
    for (size_t i = 0; i <= emulator->parameter_index; ++i) {
      select_style(emulator, emulator->parameters[i]);
    }
    return;
  }
  if (emulator->parameter_index > (command == 'H' ? 1u : 0u)) {
    return;
  }
  if (command == 'A' || command == 'B' || command == 'C' || command == 'D' ||
      command == 'G' || command == 'H' || command == 'J' || command == 'K') {
    emulator->wrap_pending = false;
  }

  switch (command) {
  case 'A':
    emulator->cursor_row = count > emulator->cursor_row ?
        0 : emulator->cursor_row - count;
    break;
  case 'B':
    emulator->cursor_row = count >= emulator->rows - emulator->cursor_row ?
        emulator->rows - 1 : emulator->cursor_row + count;
    break;
  case 'C':
    emulator->cursor_column = count >= emulator->columns - emulator->cursor_column ?
        emulator->columns - 1 : emulator->cursor_column + count;
    break;
  case 'D':
    emulator->cursor_column = count > emulator->cursor_column ?
        0 : emulator->cursor_column - count;
    break;
  case 'G':
    emulator->cursor_column = count > emulator->columns ? emulator->columns - 1 : count - 1;
    break;
  case 'H': {
    size_t column = emulator->parameters[1] ? emulator->parameters[1] : 1;
    emulator->cursor_row = count > emulator->rows ? emulator->rows - 1 : count - 1;
    emulator->cursor_column = column > emulator->columns ? emulator->columns - 1 : column - 1;
    break;
  }
  case 'K':
    if (parameter == 0) {
      erase_cells(emulator, cell, (emulator->cursor_row + 1) * emulator->columns);
    } else if (parameter == 1) {
      erase_cells(emulator, emulator->cursor_row * emulator->columns, cell + 1);
    } else if (parameter == 2) {
      erase_cells(emulator, emulator->cursor_row * emulator->columns,
          (emulator->cursor_row + 1) * emulator->columns);
    }
    break;
  case 'J':
    if (parameter == 0) {
      erase_cells(emulator, cell, cells);
    } else if (parameter == 1) {
      erase_cells(emulator, 0, cell + 1);
    } else if (parameter == 2) {
      erase_cells(emulator, 0, cells);
    }
    break;
  }
}

static void put_byte(struct mux_emulator *emulator, unsigned char byte)
{
  if (byte == 0x1b) {
    emulator->escape_state = MUX_ESCAPE;
    emulator->parameter_index = 0;
    emulator->private_csi = false;
    memset(emulator->parameters, 0, sizeof(emulator->parameters));
    return;
  }
  if (byte == '\n' || byte == '\r' || byte == '\b' || byte == '\t') {
    emulator->escape_state = MUX_TEXT;
    emulator->wrap_pending = false;
    if (byte == '\n') {
      newline(emulator);
    } else if (byte == '\r') {
      emulator->cursor_column = 0;
    } else if (byte == '\t') {
      size_t next = emulator->cursor_column + emulator->tab_width -
          emulator->cursor_column % emulator->tab_width;
      emulator->cursor_column = next < emulator->columns ? next : emulator->columns - 1;
    } else if (emulator->cursor_column) {
      --emulator->cursor_column;
    }
    return;
  }

  if (emulator->escape_state == MUX_ESCAPE) {
    emulator->escape_state = byte == '[' ? MUX_CSI_ENTRY : MUX_TEXT;
    return;
  }
  if (emulator->escape_state == MUX_CSI_ENTRY || emulator->escape_state == MUX_CSI ||
      emulator->escape_state == MUX_CSI_IGNORE) {
    if (emulator->escape_state == MUX_CSI_ENTRY) {
      emulator->escape_state = MUX_CSI;
      if (byte == '?') {
        emulator->private_csi = true;
        return;
      }
    }
    if (byte >= 0x40 && byte <= 0x7e) {
      if (emulator->escape_state == MUX_CSI) {
        execute_csi(emulator, byte);
      }
      emulator->escape_state = MUX_TEXT;
    } else if (emulator->escape_state == MUX_CSI) {
      unsigned value = emulator->parameters[emulator->parameter_index];
      if (byte >= '0' && byte <= '9' && value * 10 + byte - '0' <= UINT16_MAX) {
        emulator->parameters[emulator->parameter_index] = value * 10 + byte - '0';
      } else if (byte == ';' && emulator->parameter_index + 1 < MUX_CSI_PARAMETERS) {
        ++emulator->parameter_index;
      } else {
        emulator->escape_state = MUX_CSI_IGNORE;
      }
    }
    return;
  }
  if (byte < 0x20 || byte == 0x7f) {
    return;
  }

  if (emulator->wrap_pending) {
    newline(emulator);
  }
  write_cell(emulator, emulator->cursor_row * emulator->columns + emulator->cursor_column,
      (struct mux_cell) {
        .character = byte,
        .foreground = emulator->foreground,
        .background = emulator->background,
        .reverse = emulator->reverse,
      });
  if (emulator->cursor_column == emulator->columns - 1) {
    emulator->wrap_pending = true;
  } else {
    ++emulator->cursor_column;
  }
}

void mux_emulator_feed(struct mux_emulator *emulator, const void *bytes, size_t length)
{
  const unsigned char *data = bytes;
  for (size_t i = 0; i < length; ++i) {
    put_byte(emulator, data[i]);
  }
}

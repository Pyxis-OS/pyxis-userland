#include "emulator.h"

#include <abi/terminal.h>
#include <stdlib.h>
#include <string.h>

static bool valid_geometry(size_t columns, size_t rows)
{
  return columns && columns <= TERMINAL_COLUMNS_MAX &&
      rows && rows <= TERMINAL_ROWS_MAX;
}

static struct terminal_cell blank_cell(const struct mux_emulator *emulator)
{
  return (struct terminal_cell) {
    .glyph = ' ',
    .foreground = emulator->style.foreground,
    .background = emulator->style.background,
    .attributes = 0,
  };
}

static void fill_cells(struct terminal_cell *cells, size_t count, struct terminal_cell cell)
{
  for (size_t i = 0; i < count; ++i) {
    cells[i] = cell;
  }
}

void mux_emulator_clear_selection(struct mux_emulator *emulator)
{
  emulator->selection = (struct mux_selection){0};
}

static const struct terminal_cell *selected_row(const struct mux_emulator *emulator,
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
      const struct terminal_cell *cells = selected_row(emulator, row, &width);
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
        if (cells[column].glyph < ' ' || cells[column].glyph > '~') {
          free(owned);
          return CALL_BAD_REQUEST;
        }
      }
      while (end > begin && cells[end - 1].glyph == ' ') {
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
          owned[at++] = cells[column].glyph;
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

static void write_cell(struct mux_emulator *emulator, size_t index, struct terminal_cell cell)
{
  struct mux_selection_point point = {
    .row = emulator->scrolled_rows + index / emulator->columns,
    .column = index % emulator->columns,
  };
  if (emulator->cells[index].glyph != cell.glyph &&
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
    .style = {.foreground = TERMINAL_COLOR_DEFAULT, .background = TERMINAL_COLOR_DEFAULT},
    .tab_width = 8,
    .history_stride = columns,
    .region_bottom = rows - 1,
    .saved = {
      {.style = {.foreground = TERMINAL_COLOR_DEFAULT, .background = TERMINAL_COLOR_DEFAULT}},
      {.style = {.foreground = TERMINAL_COLOR_DEFAULT, .background = TERMINAL_COLOR_DEFAULT}},
    },
  };
  initial.cells = calloc(rows * columns, sizeof(*initial.cells));
  initial.other_cells = calloc(rows * columns, sizeof(*initial.other_cells));
  initial.history = calloc(MUX_HISTORY_ROWS * columns, sizeof(*initial.history));
  initial.history_widths = calloc(MUX_HISTORY_ROWS, sizeof(*initial.history_widths));
  if (!initial.cells || !initial.other_cells || !initial.history || !initial.history_widths) {
    mux_emulator_destroy(&initial);
    return false;
  }
  fill_cells(initial.cells, rows * columns, blank_cell(&initial));
  fill_cells(initial.other_cells, rows * columns, blank_cell(&initial));
  *emulator = initial;
  return true;
}

void mux_emulator_destroy(struct mux_emulator *emulator)
{
  free(emulator->cells);
  free(emulator->other_cells);
  free(emulator->history);
  free(emulator->history_widths);
  memset(emulator, 0, sizeof(*emulator));
}

static size_t kept_first_row(size_t cursor_row, size_t rows)
{
  return cursor_row >= rows ? cursor_row - rows + 1 : 0;
}

/* Copy a screen into new cells, keeping the rows from first_row. */
static void copy_screen(struct terminal_cell *cells, size_t columns, size_t rows,
    const struct terminal_cell *old_cells, size_t old_columns, size_t old_rows,
    size_t first_row, struct terminal_cell blank)
{
  fill_cells(cells, rows * columns, blank);
  size_t retained_rows = old_rows - first_row;
  if (retained_rows > rows) {
    retained_rows = rows;
  }
  size_t retained_columns = old_columns < columns ? old_columns : columns;
  for (size_t i = 0; i < retained_rows; ++i) {
    memcpy(cells + i * columns, old_cells + (first_row + i) * old_columns,
        retained_columns * sizeof(*cells));
  }
}

bool mux_emulator_resize(struct mux_emulator *emulator, size_t columns, size_t rows)
{
  if (!valid_geometry(columns, rows)) {
    return false;
  }
  if (columns == emulator->columns && rows == emulator->rows) {
    return true;
  }

  struct terminal_cell *cells = calloc(rows * columns, sizeof(*cells));
  struct terminal_cell *other_cells = calloc(rows * columns, sizeof(*other_cells));
  if (!cells || !other_cells) {
    free(cells);
    free(other_cells);
    return false;
  }
  struct terminal_cell *history = emulator->history;
  if (columns > emulator->history_stride) {
    history = calloc(MUX_HISTORY_ROWS * columns, sizeof(*history));
    if (!history) {
      free(cells);
      free(other_cells);
      return false;
    }
    for (size_t i = 0; i < emulator->history_count; ++i) {
      size_t slot = (emulator->history_start + i) % MUX_HISTORY_ROWS;
      memcpy(history + slot * columns,
          emulator->history + slot * emulator->history_stride,
          emulator->history_widths[slot] * sizeof(*history));
    }
  }

  /* The hidden screen keeps the rows around its saved cursor. */
  struct terminal_cell blank = blank_cell(emulator);
  struct mux_saved_cursor *hidden = &emulator->saved[!emulator->alternate];
  size_t first_row = kept_first_row(emulator->cursor_row, rows);
  size_t hidden_first_row = kept_first_row(hidden->row, rows);
  copy_screen(cells, columns, rows, emulator->cells, emulator->columns, emulator->rows,
      first_row, blank);
  copy_screen(other_cells, columns, rows, emulator->other_cells, emulator->columns,
      emulator->rows, hidden_first_row, blank);

  free(emulator->cells);
  free(emulator->other_cells);
  emulator->cells = cells;
  emulator->other_cells = other_cells;
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
  for (size_t screen = 0; screen < 2; ++screen) {
    struct mux_saved_cursor *saved = &emulator->saved[screen];
    size_t kept = screen == emulator->alternate ? first_row : hidden_first_row;
    saved->row -= saved->row < kept ? saved->row : kept;
    if (saved->row >= rows) {
      saved->row = rows - 1;
    }
    if (saved->column >= columns) {
      saved->column = columns - 1;
    }
  }
  emulator->region_top = 0;
  emulator->region_bottom = rows - 1;
  emulator->wrap_pending = false;
  mux_emulator_clear_selection(emulator);
  return true;
}

size_t mux_emulator_history_rows(const struct mux_emulator *emulator)
{
  return emulator->alternate ? 0 : emulator->history_count;
}

const struct terminal_cell *mux_emulator_row(const struct mux_emulator *emulator,
    size_t scrollback_offset, size_t visible_row, size_t *width)
{
  *width = 0;
  if (visible_row >= emulator->rows) {
    return NULL;
  }
  size_t history_rows = mux_emulator_history_rows(emulator);
  if (scrollback_offset > history_rows) {
    scrollback_offset = history_rows;
  }
  size_t row = history_rows - scrollback_offset + visible_row;
  if (row < history_rows) {
    size_t slot = (emulator->history_start + row) % MUX_HISTORY_ROWS;
    *width = emulator->history_widths[slot];
    return emulator->history + slot * emulator->history_stride;
  }
  *width = emulator->columns;
  return emulator->cells + (row - history_rows) * emulator->columns;
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
  if (scrollback_offset > mux_emulator_history_rows(emulator)) {
    scrollback_offset = mux_emulator_history_rows(emulator);
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
  if (scrollback_offset > mux_emulator_history_rows(emulator)) {
    scrollback_offset = mux_emulator_history_rows(emulator);
  }
  return emulator->selection.active && selection_contains(emulator, (struct mux_selection_point){
    .row = emulator->scrolled_rows - scrollback_offset + row,
    .column = column,
  });
}

/* Move screen row `row` into history, as the oldest-evicting ring requires. */
static void push_history(struct mux_emulator *emulator, size_t row)
{
  size_t slot;
  if (emulator->history_count == MUX_HISTORY_ROWS) {
    slot = emulator->history_start;
    emulator->history_start = (emulator->history_start + 1) % MUX_HISTORY_ROWS;
  } else {
    slot = (emulator->history_start + emulator->history_count) % MUX_HISTORY_ROWS;
    ++emulator->history_count;
  }
  memcpy(emulator->history + slot * emulator->history_stride,
      emulator->cells + row * emulator->columns,
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
}

/* Scroll rows top..bottom (inclusive) up by count. Rows leaving the top of the
 * primary screen enter history; other scrolling discards them. */
static void scroll_up(struct mux_emulator *emulator, size_t top, size_t bottom, size_t count)
{
  size_t rows = bottom - top + 1;
  if (count > rows) {
    count = rows;
  }
  if (!emulator->alternate && top == 0) {
    for (size_t i = 0; i < count; ++i) {
      push_history(emulator, i);
    }
    /* Selection rows are absolute; they follow only whole-screen scrolling. */
    if (bottom != emulator->rows - 1) {
      mux_emulator_clear_selection(emulator);
    }
  } else {
    mux_emulator_clear_selection(emulator);
  }
  memmove(emulator->cells + top * emulator->columns,
      emulator->cells + (top + count) * emulator->columns,
      (rows - count) * emulator->columns * sizeof(*emulator->cells));
  /* Native scrolling fills new rows with current colors and no attributes. */
  fill_cells(emulator->cells + (bottom + 1 - count) * emulator->columns,
      count * emulator->columns, blank_cell(emulator));
}

/* Scroll rows top..bottom (inclusive) down by count, blanking the top. */
static void scroll_down(struct mux_emulator *emulator, size_t top, size_t bottom, size_t count)
{
  size_t rows = bottom - top + 1;
  if (count > rows) {
    count = rows;
  }
  mux_emulator_clear_selection(emulator);
  memmove(emulator->cells + (top + count) * emulator->columns,
      emulator->cells + top * emulator->columns,
      (rows - count) * emulator->columns * sizeof(*emulator->cells));
  fill_cells(emulator->cells + top * emulator->columns, count * emulator->columns,
      blank_cell(emulator));
}

/* LF: the bottom margin scrolls the region; below it the screen edge stops. */
static void newline(struct mux_emulator *emulator)
{
  emulator->wrap_pending = false;
  emulator->cursor_column = 0;
  if (emulator->cursor_row == emulator->region_bottom) {
    scroll_up(emulator, emulator->region_top, emulator->region_bottom, 1);
  } else if (emulator->cursor_row + 1 < emulator->rows) {
    ++emulator->cursor_row;
  }
}

/* ESC M: the top margin scrolls the region down; above it the screen edge stops. */
static void reverse_index(struct mux_emulator *emulator)
{
  emulator->wrap_pending = false;
  if (emulator->cursor_row == emulator->region_top) {
    scroll_down(emulator, emulator->region_top, emulator->region_bottom, 1);
  } else if (emulator->cursor_row) {
    --emulator->cursor_row;
  }
}

static void save_cursor(struct mux_emulator *emulator)
{
  emulator->saved[emulator->alternate] = (struct mux_saved_cursor){
    .row = emulator->cursor_row,
    .column = emulator->cursor_column,
    .style = emulator->style,
    .wrap_pending = emulator->wrap_pending,
  };
}

static void restore_cursor(struct mux_emulator *emulator)
{
  const struct mux_saved_cursor *saved = &emulator->saved[emulator->alternate];
  emulator->cursor_row = saved->row < emulator->rows ? saved->row : emulator->rows - 1;
  emulator->cursor_column = saved->column < emulator->columns ?
      saved->column : emulator->columns - 1;
  emulator->style = saved->style;
  emulator->wrap_pending = saved->wrap_pending &&
      emulator->cursor_column == emulator->columns - 1;
}

/* CSI ? 1049 h/l. Entering saves the primary cursor and shows a cleared
 * alternate screen; leaving restores the primary screen and cursor. The scroll
 * region resets either way. */
static void select_screen(struct mux_emulator *emulator, bool alternate)
{
  if (emulator->alternate == alternate) {
    return;
  }
  if (alternate) {
    save_cursor(emulator);
  }
  struct terminal_cell *cells = emulator->cells;
  emulator->cells = emulator->other_cells;
  emulator->other_cells = cells;
  emulator->alternate = alternate;
  mux_emulator_clear_selection(emulator);
  emulator->region_top = 0;
  emulator->region_bottom = emulator->rows - 1;
  emulator->wrap_pending = false;
  if (alternate) {
    fill_cells(emulator->cells, emulator->rows * emulator->columns,
        blank_cell(emulator));
    emulator->cursor_row = 0;
    emulator->cursor_column = 0;
  } else {
    restore_cursor(emulator);
  }
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
  struct terminal_cell blank = blank_cell(emulator);
  for (size_t i = first; i < end; ++i) {
    write_cell(emulator, i, blank);
  }
}

static void execute_csi(struct mux_emulator *emulator, unsigned char command)
{
  unsigned parameter = emulator->parameters[0];
  size_t count = parameter ? parameter : 1;
  size_t cell = emulator->cursor_row * emulator->columns + emulator->cursor_column;
  size_t cells = emulator->rows * emulator->columns;

  bool in_region = emulator->cursor_row >= emulator->region_top &&
      emulator->cursor_row <= emulator->region_bottom;

  if (emulator->private_csi) {
    if (emulator->parameter_index == 0 && (command == 'h' || command == 'l')) {
      if (parameter == 25) {
        emulator->cursor_visible = command == 'h';
      } else if (parameter == 1049) {
        select_screen(emulator, command == 'h');
      }
    }
    return;
  }
  if (command == 'm') {
    terminal_sgr_apply(&emulator->style, emulator->parameters,
        emulator->parameter_present, emulator->parameter_index + 1);
    return;
  }
  if (command == 'r') {
    /* DECSTBM: one-based rows; zero or missing selects the screen edge. */
    if (emulator->parameter_index > 1) {
      return;
    }
    size_t top = parameter ? parameter : 1;
    size_t bottom = emulator->parameters[1] ? emulator->parameters[1] : emulator->rows;
    if (top < bottom && bottom <= emulator->rows) {
      emulator->region_top = top - 1;
      emulator->region_bottom = bottom - 1;
      emulator->cursor_row = 0;
      emulator->cursor_column = 0;
      emulator->wrap_pending = false;
    }
    return;
  }
  if (emulator->parameter_index > (command == 'H' ? 1u : 0u)) {
    return;
  }
  if ((command == 's' || command == 'u') && parameter) {
    return;
  }
  if (command == 'A' || command == 'B' || command == 'C' || command == 'D' ||
      command == 'G' || command == 'H' || command == 'J' || command == 'K' ||
      command == 'L' || command == 'M') {
    emulator->wrap_pending = false;
  }

  switch (command) {
  case 'A': {
    /* Inside the region the top margin stops the cursor, as in xterm. */
    size_t limit = in_region ? emulator->region_top : 0;
    emulator->cursor_row = count > emulator->cursor_row - limit ?
        limit : emulator->cursor_row - count;
    break;
  }
  case 'B': {
    size_t limit = in_region ? emulator->region_bottom : emulator->rows - 1;
    emulator->cursor_row = count > limit - emulator->cursor_row ?
        limit : emulator->cursor_row + count;
    break;
  }
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
  case 'L':
    /* Insert and delete lines act inside the region and return to column one. */
    if (in_region) {
      scroll_down(emulator, emulator->cursor_row, emulator->region_bottom, count);
      emulator->cursor_column = 0;
    }
    break;
  case 'M':
    if (in_region) {
      scroll_up(emulator, emulator->cursor_row, emulator->region_bottom, count);
      emulator->cursor_column = 0;
    }
    break;
  case 's':
    save_cursor(emulator);
    break;
  case 'u':
    restore_cursor(emulator);
    break;
  }
}

static void put_byte(struct mux_emulator *emulator, unsigned char byte)
{
  if (byte == 0x1b) {
    emulator->escape_state = MUX_ESCAPE;
    emulator->parameter_index = 0;
    emulator->parameter_present = 0;
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
    emulator->escape_state = MUX_TEXT;
    if (byte == '[') {
      emulator->escape_state = MUX_CSI_ENTRY;
    } else if (byte == '(' || byte == ')' || byte == '*' || byte == '+') {
      emulator->escape_state = MUX_CHARSET;
    } else if (byte == '7') {
      save_cursor(emulator);
    } else if (byte == '8') {
      restore_cursor(emulator);
    } else if (byte == 'M') {
      reverse_index(emulator);
    }
    return;
  }
  if (emulator->escape_state == MUX_CHARSET) {
    /* Only the default set exists; the designation is consumed. */
    emulator->escape_state = MUX_TEXT;
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
        emulator->parameter_present |= (uint16_t)(1u << emulator->parameter_index);
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
      (struct terminal_cell) {
        .glyph = byte,
        .foreground = emulator->style.foreground,
        .background = emulator->style.background,
        .attributes = emulator->style.attributes,
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

#ifndef MUX_EMULATOR_H
#define MUX_EMULATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <abi/syscall.h>

#define MUX_HISTORY_ROWS 1024
#define MUX_CSI_PARAMETERS 4

struct mux_cell {
  unsigned char character;
  signed char foreground;
  signed char background;
  bool reverse;
};

struct mux_selection_point {
  uint64_t row;
  size_t column;
};

struct mux_selection {
  bool active, pending;
  size_t columns;
  struct mux_selection_point anchor, end;
};

enum mux_escape_state {
  MUX_TEXT,
  MUX_ESCAPE,
  MUX_CSI_ENTRY,
  MUX_CSI,
  MUX_CSI_IGNORE,
};

struct mux_emulator {
  size_t columns;
  size_t rows;
  size_t cursor_row;
  size_t cursor_column;
  bool cursor_visible;
  size_t history_count;
  uint64_t scrolled_rows; /* Saturates at UINT64_MAX. */
  struct mux_selection selection;

  /* Owned storage. History retains each row's original width without reflow.
   * The maximum width seen supplies a stride, so output never allocates. */
  struct mux_cell *cells;
  struct mux_cell *history;
  size_t *history_widths;
  size_t history_start;
  size_t history_stride;

  signed char foreground;
  signed char background;
  bool reverse;
  bool wrap_pending;
  unsigned tab_width;
  enum mux_escape_state escape_state;
  uint16_t parameters[MUX_CSI_PARAMETERS];
  size_t parameter_index;
  bool private_csi;
};

/* Geometry follows the terminal-session ABI's nonzero bounds. Palette indices
 * are 0..15; -1 denotes the terminal's separate foreground/background default.
 * Init takes uninitialized storage. Failed resize preserves all old state. */
bool mux_emulator_init(struct mux_emulator *emulator, size_t columns, size_t rows);
bool mux_emulator_resize(struct mux_emulator *emulator, size_t columns, size_t rows);
void mux_emulator_destroy(struct mux_emulator *emulator);
void mux_emulator_feed(struct mux_emulator *emulator, const void *bytes, size_t length);
void mux_emulator_fresh_line(struct mux_emulator *emulator);
void mux_emulator_set_tab_width(struct mux_emulator *emulator, unsigned columns);

/* Offset zero is the live screen. Larger offsets move back through history,
 * clamped to history_count. The borrowed row survives until feed/resize/destroy;
 * its original width can differ from the current screen width. */
const struct mux_cell *mux_emulator_row(const struct mux_emulator *emulator,
    size_t scrollback_offset, size_t visible_row, size_t *width);
/* Selection names retained rows, never borrowed cell pointers. Glyph changes
 * and eviction invalidate it; colors and unrelated output do not. A press
 * records a pending anchor; only a held move to another cell activates it. */
bool mux_emulator_select(struct mux_emulator *emulator, size_t scrollback_offset,
    size_t row, size_t column, size_t visible_columns, bool extend, bool activate);
bool mux_emulator_selected(const struct mux_emulator *emulator,
    size_t scrollback_offset, size_t row, size_t column);
void mux_emulator_clear_selection(struct mux_emulator *emulator);
/* Freeze printable selected glyphs into owned text. Trim trailing spaces per
 * physical row and join rows with LF. Caller frees the returned allocation. */
enum call_status mux_emulator_copy_selection(const struct mux_emulator *emulator,
    size_t limit, char **text, size_t *length);

#endif

#ifndef MUX_H
#define MUX_H

#include "emulator.h"
#include "layout.h"
#include "session.h"
#include <abi/process.h>
#include <abi/wait.h>
#include <term.h>
#include <terminal_pointer.h>

_Static_assert(MUX_PANES * 2 + 3 <= WAIT_MAX_INTERESTS,
    "pane output/lifecycle, outer input/output and pointer must fit one native wait");

struct mux_pane {
  bool used, root_done, group_done, output_eof, remove_when_done;
  struct mux_session session;
  struct mux_emulator emulator;
  struct process_result result;
  size_t scrollback;
  int32_t scroll_remainder;
  bool browsing;
  unsigned char pending[256];
  size_t pending_size;
};

#define MUX_RENDER_BYTES 8192
#define MUX_RENDER_CELLS 256

struct mux_output {
  unsigned char bytes[MUX_RENDER_BYTES];
  size_t size, at;
  struct terminal_cell cells[MUX_RENDER_CELLS];
  size_t ends[MUX_RENDER_CELLS];
  size_t first, count, committed;
  bool reset, finish, blocked;
};

struct mux {
  struct terminal terminal;
  handle_t clock;
  handle_t pointer;
  handle_t clipboard_local, clipboard_shared;
  bool pointer_owned, dragging;
  unsigned selection_pane;
  int64_t drag_column, drag_row;
  uint32_t pointer_buttons;
  struct terminal_pointer_geometry pointer_geometry;
  struct console_size_reply geometry;
  struct mux_layout layout;
  struct mux_rect rectangles[MUX_PANES];
  struct mux_pane panes[MUX_PANES];
  unsigned focused, tab_width;
  /* Memory each pane may hold as scrollback (docs/userland/multiplexer.md). */
  size_t history_bytes;
  bool collapsed, zoomed, prefix, confirm, dirty;
  enum call_status error;
  char notice[128];
  unsigned char input[256];
  size_t input_size, input_at;
  unsigned char escape[16];
  size_t escape_size;
  uint64_t escape_deadline;
  struct terminal_cell *frame, *previous;
  size_t frame_columns, frame_rows;
  bool frame_valid, render_active, render_again;
  size_t render_scan;
  uint64_t rendered_pointer_mapping;
  struct mux_output output;
};

enum call_status mux_render(struct mux *mux, bool *progress);
void mux_pointer_clear_selection(struct mux *mux);
void mux_pointer_clear_scroll(struct mux *mux);
enum call_status mux_pointer_advance_view(struct mux *mux);
enum call_status mux_pointer_change_view(struct mux *mux);
void mux_pointer_drain(struct mux *mux);
void mux_clipboard_action(struct mux *mux, const struct pointer_event *event);

/* Cell, cursor and selection redraws keep their existing coordinate meaning.
 * Existing view barriers advance the identity when that meaning changes. */
static inline bool mux_pointer_view_pending(const struct mux *mux)
{
  return mux->pointer_owned && mux->pointer_geometry.surface.mapping_identity !=
      mux->rendered_pointer_mapping;
}

static inline struct mux_rect mux_viewport(const struct mux *mux)
{
  size_t columns = mux->geometry.columns, rows = mux->geometry.rows;
  if (columns > TERMINAL_COLUMNS_MAX) {
    columns = TERMINAL_COLUMNS_MAX;
  }
  if (rows > TERMINAL_ROWS_MAX + 2) {
    rows = TERMINAL_ROWS_MAX + 2;
  }
  return (struct mux_rect){.width = columns, .height = rows};
}

#endif

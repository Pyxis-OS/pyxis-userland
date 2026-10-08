#ifndef MUX_H
#define MUX_H

#include "emulator.h"
#include "layout.h"
#include "session.h"
#include <abi/process.h>
#include <abi/wait.h>
#include <term.h>
#include <terminal_pointer.h>

_Static_assert(MUX_PANES * 2 + 2 <= WAIT_MAX_INTERESTS,
    "pane output/lifecycle, outer input and pointer must fit one native wait");

struct mux_pane {
  bool used, root_done, group_done, output_eof, remove_when_done;
  struct mux_session session;
  struct mux_emulator emulator;
  struct process_result result;
  size_t scrollback;
  bool browsing;
  unsigned char pending[256];
  size_t pending_size;
};

struct mux {
  struct terminal terminal;
  handle_t clock;
  handle_t pointer;
  bool pointer_owned, dragging;
  unsigned selection_pane;
  uint32_t pointer_buttons;
  struct terminal_pointer_geometry pointer_geometry;
  struct console_size_reply geometry;
  struct mux_layout layout;
  struct mux_rect rectangles[MUX_PANES];
  struct mux_pane panes[MUX_PANES];
  unsigned focused, tab_width;
  bool collapsed, prefix, confirm, dirty;
  enum call_status error;
  char notice[128];
  unsigned char input[256];
  size_t input_size, input_at;
  unsigned char escape[16];
  size_t escape_size;
  uint64_t escape_deadline;
  struct mux_cell *frame, *previous;
  size_t frame_columns, frame_rows;
  bool frame_valid;
};

enum call_status mux_render(struct mux *mux);
void mux_pointer_clear_selection(struct mux *mux);
enum call_status mux_pointer_advance_view(struct mux *mux);
enum call_status mux_pointer_change_view(struct mux *mux);
void mux_pointer_drain(struct mux *mux);

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

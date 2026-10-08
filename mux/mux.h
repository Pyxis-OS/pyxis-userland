#ifndef MUX_H
#define MUX_H

#include "emulator.h"
#include "layout.h"
#include "session.h"
#include <abi/process.h>
#include <term.h>

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

#endif

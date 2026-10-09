#include "mux.h"
#include <clock.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUX_ESCAPE_NS UINT64_C(100000000)
#define MUX_IDLE_NS UINT64_C(30000000000)
#define MUX_DRAIN_RECORDS 8

static void notice(struct mux *mux, const char *text)
{
  snprintf(mux->notice, sizeof(mux->notice), "%s", text);
  mux->dirty = true;
}

static size_t pane_columns(struct mux_rect rect)
{
  return rect.width < MUX_MIN_COLUMNS ? MUX_MIN_COLUMNS : rect.width;
}

static size_t pane_rows(struct mux_rect rect)
{
  size_t rows = rect.height > 1 ? rect.height - 1 : 0;
  return rows < MUX_MIN_ROWS ? MUX_MIN_ROWS : rows;
}

static enum call_status place(struct mux *mux)
{
  enum call_status status = mux_pointer_change_view(mux);
  if (status != CALL_OK) {
    return status;
  }
  struct mux_rect view = mux_viewport(mux);
  size_t columns = view.width, rows = view.height;
  size_t area_rows = rows > 1 ? rows - 1 : 0;
  mux->collapsed = !mux_layout_place(&mux->layout, columns, area_rows, mux->rectangles);
  if (mux->collapsed) {
    memset(mux->rectangles, 0, sizeof(mux->rectangles));
    mux->rectangles[mux->focused] = (struct mux_rect){.width = columns, .height = area_rows};
  }
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    struct mux_pane *pane = &mux->panes[i];
    const struct mux_rect *rect = &mux->rectangles[i];
    if (!pane->used || !rect->width || !rect->height) {
      continue;
    }
    size_t width = pane_columns(*rect), height = pane_rows(*rect);
    if (!mux_emulator_resize(&pane->emulator, width, height)) {
      return CALL_NO_MEMORY;
    }
    if (!pane->root_done) {
      status = terminal_resize(pane->session.attachment, width, height);
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  mux->dirty = true;
  return CALL_OK;
}

static bool start_pane(struct mux *mux, unsigned slot, struct mux_rect rect)
{
  struct mux_pane *pane = &mux->panes[slot];
  size_t width = pane_columns(rect), height = pane_rows(rect);
  if (!mux_emulator_init(&pane->emulator, width, height)) {
    notice(mux, "Cannot allocate pane storage");
    return false;
  }
  mux_emulator_set_tab_width(&pane->emulator, mux->tab_width);
  enum call_status status = mux_session_start(width, height, mux->tab_width, &pane->session);
  if (status != CALL_OK) {
    mux_emulator_destroy(&pane->emulator);
    snprintf(mux->notice, sizeof(mux->notice), "Cannot start pane (status %u)", status);
    mux->dirty = true;
    return false;
  }
  pane->used = true;
  return true;
}

static void split(struct mux *mux, enum mux_axis axis)
{
  unsigned slot = 0;
  while (slot < MUX_PANES && mux->panes[slot].used) {
    ++slot;
  }
  if (slot == MUX_PANES) {
    notice(mux, "Eight panes already open");
    return;
  }
  struct mux_layout next = mux->layout;
  struct mux_rect rectangles[MUX_PANES];
  struct mux_rect view = mux_viewport(mux);
  if (!mux_layout_split(&next, mux->focused, (int)slot, axis) ||
      !mux_layout_place(&next, view.width, view.height ? view.height - 1 : 0, rectangles)) {
    notice(mux, "Not enough room for 12 columns and 4 content rows per pane");
    return;
  }
  if (start_pane(mux, slot, rectangles[slot])) {
    mux->layout = next;
    mux->focused = slot;
    mux->notice[0] = 0;
    mux->error = place(mux);
  }
}

static bool any_panes(const struct mux *mux)
{
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    if (mux->panes[i].used) {
      return true;
    }
  }
  return false;
}

static void dismiss(struct mux *mux, unsigned slot)
{
  struct mux_pane *pane = &mux->panes[slot];
  mux_session_release(&pane->session);
  mux_emulator_destroy(&pane->emulator);
  *pane = (struct mux_pane){0};
  mux_layout_remove(&mux->layout, (int)slot);
  if (slot == mux->focused) {
    for (unsigned i = 0; i < MUX_PANES; ++i) {
      if (mux->panes[i].used) {
        mux->focused = i;
        break;
      }
    }
  }
  mux->error = place(mux);
}

static void focus(struct mux *mux, enum mux_axis axis, bool forward)
{
  int selected = mux_layout_neighbor(mux->rectangles, (int)mux->focused, axis, forward);
  if (mux->collapsed) {
    for (unsigned step = 1; step < MUX_PANES; ++step) {
      unsigned slot = forward ? (mux->focused + step) % MUX_PANES :
          (mux->focused + MUX_PANES - step) % MUX_PANES;
      if (mux->panes[slot].used) {
        selected = (int)slot;
        break;
      }
    }
  }
  if (mux->focused != (unsigned)selected) {
    mux->error = mux_pointer_advance_view(mux);
    if (mux->error != CALL_OK) {
      return;
    }
  }
  mux->focused = (unsigned)selected;
  if (mux->collapsed) {
    mux->error = place(mux);
  } else {
    mux->dirty = true;
  }
}

static void command(struct mux *mux, unsigned key)
{
  struct mux_pane *pane = &mux->panes[mux->focused];
  mux->dirty = true;
  if (pane->browsing) {
    if (key != 27 && key != 'q' && key != TERM_KEY_PAGE_UP && key != TERM_KEY_UP &&
        key != TERM_KEY_PAGE_DOWN && key != TERM_KEY_DOWN && key != TERM_KEY_HOME &&
        key != TERM_KEY_END) {
      return;
    }
    mux->error = mux_pointer_change_view(mux);
    if (mux->error != CALL_OK) {
      return;
    }
    size_t page = pane->emulator.rows > 1 ? pane->emulator.rows - 1 : 1;
    if (key == 27 || key == 'q') {
      pane->browsing = false;
      pane->scrollback = 0;
    } else if (key == TERM_KEY_PAGE_UP || key == TERM_KEY_UP) {
      size_t step = key == TERM_KEY_UP ? 1 : page;
      size_t remaining = pane->emulator.history_count - pane->scrollback;
      pane->scrollback += step < remaining ? step : remaining;
    } else if (key == TERM_KEY_PAGE_DOWN || key == TERM_KEY_DOWN) {
      size_t step = key == TERM_KEY_DOWN ? 1 : page;
      pane->scrollback -= step < pane->scrollback ? step : pane->scrollback;
    } else if (key == TERM_KEY_HOME) {
      pane->scrollback = pane->emulator.history_count;
    } else if (key == TERM_KEY_END) {
      pane->scrollback = 0;
    }
    return;
  }
  switch (key) {
  case 2:
    if (pane->root_done || pane->remove_when_done) {
      notice(mux, "Pane has exited; Ctrl+B x dismisses it");
    } else {
      pane->pending[pane->pending_size++] = 2;
    }
    break;
  case '%': split(mux, MUX_LEFT_RIGHT); break;
  case '"': split(mux, MUX_TOP_BOTTOM); break;
  case 'b': case 'e':
    mux->layout.kind = key == 'b' ? MUX_BSP : MUX_EQUAL;
    mux->notice[0] = 0;
    mux->error = place(mux);
    break;
  case TERM_KEY_LEFT: focus(mux, MUX_LEFT_RIGHT, false); break;
  case TERM_KEY_RIGHT: focus(mux, MUX_LEFT_RIGHT, true); break;
  case TERM_KEY_UP: focus(mux, MUX_TOP_BOTTOM, false); break;
  case TERM_KEY_DOWN: focus(mux, MUX_TOP_BOTTOM, true); break;
  case '[':
    mux->error = mux_pointer_change_view(mux);
    if (mux->error == CALL_OK) {
      pane->browsing = true;
      pane->scrollback = 0;
    }
    break;
  case 'x':
    if (pane->root_done && pane->group_done && pane->output_eof) {
      dismiss(mux, mux->focused);
    } else {
      mux->confirm = true;
    }
    break;
  case 27: break;
  default: notice(mux, "Unknown prefix command"); break;
  }
}

static unsigned decoded_escape(const unsigned char *bytes, size_t length)
{
  if (length < 3 || (bytes[1] != '[' && bytes[1] != 'O')) {
    return TERM_KEY_UNKNOWN;
  }
  if (length == 3) {
    switch (bytes[2]) {
    case 'A': return TERM_KEY_UP;
    case 'B': return TERM_KEY_DOWN;
    case 'C': return TERM_KEY_RIGHT;
    case 'D': return TERM_KEY_LEFT;
    case 'H': return TERM_KEY_HOME;
    case 'F': return TERM_KEY_END;
    }
  }
  if (length == 4 && bytes[1] == '[' && bytes[3] == '~') {
    switch (bytes[2]) {
    case '1': case '7': return TERM_KEY_HOME;
    case '4': case '8': return TERM_KEY_END;
    case '5': return TERM_KEY_PAGE_UP;
    case '6': return TERM_KEY_PAGE_DOWN;
    }
  }
  return TERM_KEY_UNKNOWN;
}

/* Application input stays byte-exact; only mux commands/history are decoded.
 * Queued application bytes stay attached to their pane across focus changes. */
static bool consume_input(struct mux *mux, uint64_t now)
{
  bool progress = false;
  if (mux->escape_size && now >= mux->escape_deadline) {
    command(mux, mux->escape_size == 1 ? 27 : TERM_KEY_UNKNOWN);
    mux->prefix = false;
    mux->escape_size = 0;
    progress = true;
  }
  while (mux->input_at < mux->input_size && mux->error == CALL_OK && any_panes(mux)) {
    struct mux_pane *pane = &mux->panes[mux->focused];
    unsigned char byte = mux->input[mux->input_at];
    bool application = !mux->confirm && !mux->escape_size && !pane->browsing &&
        (mux->prefix ? byte == 2 : byte != 2);
    if (application && !pane->root_done && !pane->remove_when_done &&
        pane->pending_size == sizeof(pane->pending)) {
      /* Leave the byte staged until its pane has capacity; controls can still
       * proceed when they are the next bytes in the outer input stream. */
      break;
    }
    ++mux->input_at;
    progress = true;
    if (mux->confirm) {
      if (byte == 'y' || byte == 'Y') {
        mux->confirm = false;
        pane->remove_when_done = true;
        pane->pending_size = 0;
        pane->browsing = false;
        pane->scrollback = 0;
        mux->error = execution_group_terminate(pane->session.group);
        mux->dirty = true;
      } else if (byte == 'n' || byte == 'N' || byte == 27) {
        mux->confirm = false;
        mux->dirty = true;
      }
      continue;
    }
    if (mux->escape_size) {
      if (mux->escape_size == sizeof(mux->escape)) {
        mux->escape_size = 0;
        mux->prefix = false;
        command(mux, TERM_KEY_UNKNOWN);
        continue;
      }
      mux->escape[mux->escape_size++] = byte;
      mux->escape_deadline = now > UINT64_MAX - MUX_ESCAPE_NS ?
          UINT64_MAX : now + MUX_ESCAPE_NS;
      if (mux->escape_size == 2 && (byte == '[' || byte == 'O')) {
        continue;
      }
      if (byte >= 0x40 && byte <= 0x7e) {
        unsigned key = decoded_escape(mux->escape, mux->escape_size);
        mux->escape_size = 0;
        mux->prefix = false;
        command(mux, key);
      }
      continue;
    }
    if (mux->prefix || pane->browsing) {
      if (byte == 27) {
        mux->escape[0] = byte;
        mux->escape_size = 1;
        mux->escape_deadline = now > UINT64_MAX - MUX_ESCAPE_NS ?
          UINT64_MAX : now + MUX_ESCAPE_NS;
      } else {
        mux->prefix = false;
        command(mux, byte);
      }
      continue;
    }
    if (byte == 2) {
      mux->prefix = true;
      mux->notice[0] = 0;
      mux->dirty = true;
      continue;
    }
    if (pane->root_done || pane->remove_when_done) {
      notice(mux, "Pane has exited; Ctrl+B x dismisses it");
      continue;
    }
    pane->pending[pane->pending_size++] = byte;
  }
  if (mux->input_at == mux->input_size) {
    mux->input_at = mux->input_size = 0;
  }
  return progress;
}

static bool inject(struct mux *mux, struct mux_pane *pane)
{
  if (!pane->pending_size) {
    return false;
  }
  struct terminal_transfer_reply reply;
  enum call_status status = terminal_try_inject(pane->session.attachment,
      pane->pending, pane->pending_size, &reply);
  if (status == CALL_WOULD_BLOCK) {
    return false;
  }
  if (status == CALL_ENDPOINT_CLOSED) {
    pane->pending_size = 0;
    notice(mux, "Pane input closed");
    return true;
  }
  if (status != CALL_OK) {
    mux->error = status;
    return false;
  }
  pane->pending_size -= reply.length;
  memmove(pane->pending, pane->pending + reply.length, pane->pending_size);
  return true;
}

static bool drain(struct mux *mux, struct mux_pane *pane)
{
  if (pane->output_eof) {
    return false;
  }
  unsigned char bytes[TERMINAL_RECORD_MAX];
  bool progress = false;
  for (unsigned i = 0; i < MUX_DRAIN_RECORDS; ++i) {
    struct terminal_transfer_reply reply;
    enum call_status status = terminal_try_drain(pane->session.attachment, bytes, sizeof(bytes), &reply);
    if (status == CALL_WOULD_BLOCK) {
      break;
    }
    if (status != CALL_OK) {
      mux->error = status;
      break;
    }
    progress = true;
    if (!reply.length) {
      pane->output_eof = true;
      break;
    }
    struct terminal_record record;
    memcpy(&record, bytes, sizeof(record));
    uint64_t before = pane->emulator.scrolled_rows;
    uint64_t first_row = before - pane->scrollback;
    const void *payload = bytes + sizeof(record);
    if (record.type == TERMINAL_RECORD_DATA) {
      mux_emulator_feed(&pane->emulator, payload, record.length);
    } else if (record.type == TERMINAL_RECORD_FRESH_LINE) {
      mux_emulator_fresh_line(&pane->emulator);
    } else if (record.type == TERMINAL_RECORD_TAB_WIDTH) {
      uint64_t width;
      memcpy(&width, payload, sizeof(width));
      mux_emulator_set_tab_width(&pane->emulator, (unsigned)width);
    }
    if (pane->browsing && pane->scrollback) {
      uint64_t moved = pane->emulator.scrolled_rows - before;
      size_t room = pane->emulator.history_count - pane->scrollback;
      pane->scrollback += moved < room ? (size_t)moved : room;
    }
    unsigned slot = (unsigned)(pane - mux->panes);
    const struct mux_rect *rect = &mux->rectangles[slot];
    struct mux_selection *selection = &pane->emulator.selection;
    uint64_t visible_first = pane->emulator.scrolled_rows - pane->scrollback;
    size_t visible_rows = rect->height > 1 ? rect->height - 1 : 0;
    if (selection->active && (!visible_rows ||
        selection->anchor.row < visible_first || selection->end.row < visible_first ||
        selection->anchor.row - visible_first >= visible_rows ||
        selection->end.row - visible_first >= visible_rows)) {
      mux_emulator_clear_selection(&pane->emulator);
    }
    if (visible_rows && rect->width && visible_first != first_row) {
      /* Retained selected rows may survive output, but queued coordinates name
       * the previous visible rows. End the drag before advancing this view. */
      if (mux->dragging) {
        mux_emulator_clear_selection(&mux->panes[mux->selection_pane].emulator);
      }
      mux->dragging = false;
      mux->pointer_buttons = 0;
      mux->error = mux_pointer_advance_view(mux);
      if (mux->error != CALL_OK) {
        break;
      }
    }
    mux->dirty = true;
  }
  return progress;
}

static void lifecycle(struct mux *mux, struct mux_pane *pane, uint64_t events)
{
  if (!(events & WAIT_COMPLETE) || mux->error != CALL_OK) {
    return;
  }
  if (!pane->root_done) {
    mux->error = mux_pointer_advance_view(mux);
    if (mux->error != CALL_OK) {
      return;
    }
    mux->error = process_wait(pane->session.process, &pane->result);
    if (mux->error == CALL_OK) {
      pane->root_done = true;
      pane->pending_size = 0;
      mux->error = execution_group_terminate(pane->session.group);
    }
  } else {
    mux->error = execution_group_wait(pane->session.group);
    pane->group_done = mux->error == CALL_OK;
  }
  mux->dirty = true;
}

static enum call_status run(struct mux *mux)
{
  while (any_panes(mux) && mux->error == CALL_OK) {
    uint64_t now;
    mux->error = clock_now(mux->clock, &now);
    if (mux->error != CALL_OK) {
      break;
    }
    bool progress = false;
    for (unsigned i = 0; i < MUX_PANES && mux->error == CALL_OK; ++i) {
      if (mux->panes[i].used) {
        progress |= inject(mux, &mux->panes[i]);
      }
    }
    if (mux->error == CALL_OK) {
      progress |= consume_input(mux, now);
    }
    for (unsigned i = 0; i < MUX_PANES && mux->error == CALL_OK; ++i) {
      struct mux_pane *pane = &mux->panes[i];
      if (pane->used) {
        progress |= drain(mux, pane);
        if (pane->root_done && pane->group_done && pane->output_eof && pane->remove_when_done) {
          dismiss(mux, i);
          progress = true;
        }
      }
    }
    if (mux->dirty && mux->error == CALL_OK) {
      mux->error = mux_render(mux);
      mux->dirty = false;
    }
    if (mux->error != CALL_OK || !any_panes(mux)) {
      break;
    }
    struct wait_interest interests[WAIT_MAX_INTERESTS];
    uint64_t events[WAIT_MAX_INTERESTS];
    size_t output_entries[MUX_PANES], life_entries[MUX_PANES];
    bool input_capacity = !mux->input_size;
    interests[0] = (struct wait_interest){.handle = mux->terminal.input,
        .events = WAIT_RESIZED | (input_capacity ? WAIT_READABLE : 0),
        .observed_generation = mux->geometry.generation};
    size_t count = 1;
    size_t pointer_entry = SIZE_MAX;
    if (mux->pointer_owned) {
      pointer_entry = count;
      interests[count++] = (struct wait_interest){mux->pointer, WAIT_READABLE, 0};
    }
    for (unsigned i = 0; i < MUX_PANES; ++i) {
      struct mux_pane *pane = &mux->panes[i];
      output_entries[i] = life_entries[i] = SIZE_MAX;
      if (!pane->used) {
        continue;
      }
      uint64_t output_events = pane->output_eof ? 0 : WAIT_READABLE;
      if (pane->pending_size) {
        output_events |= WAIT_WRITABLE;
      }
      if (output_events) {
        output_entries[i] = count;
        interests[count++] = (struct wait_interest){pane->session.attachment, output_events, 0};
      }
      if (!pane->group_done) {
        life_entries[i] = count;
        interests[count++] = (struct wait_interest){pane->root_done ? pane->session.group :
            pane->session.process, WAIT_COMPLETE, 0};
      }
    }
    uint64_t deadline = now > UINT64_MAX - MUX_IDLE_NS ? UINT64_MAX : now + MUX_IDLE_NS;
    if (mux->escape_size && mux->escape_deadline < deadline) {
      deadline = mux->escape_deadline;
    }
    enum call_status status = wait_many(interests, count, progress ? 0 : deadline, events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (events[0] & WAIT_RESIZED) {
      mux->error = term_geometry(&mux->terminal, &mux->geometry);
      if (mux->error == CALL_OK) {
        mux->error = place(mux);
      }
    }
    if (pointer_entry != SIZE_MAX && mux->error == CALL_OK) {
      if (events[pointer_entry] & WAIT_ERROR) {
        return CALL_ENDPOINT_CLOSED;
      }
      if (events[pointer_entry] & WAIT_READABLE) {
        mux_pointer_drain(mux);
      }
    }
    if (input_capacity && (events[0] & (WAIT_READABLE | WAIT_ERROR))) {
      status = term_read_timeout(&mux->terminal, mux->input, sizeof(mux->input), 0, &mux->input_size);
      if (status == CALL_INPUT_LOST) {
        mux->input_at = mux->input_size = mux->escape_size = 0;
        mux->prefix = mux->confirm = false;
        notice(mux, "Outer input lost; retry the command");
      } else if (status == CALL_OK && !mux->input_size) {
        return CALL_OK;
      } else if (status != CALL_OK && status != CALL_TIMED_OUT) {
        return status;
      }
    }
    for (unsigned i = 0; i < MUX_PANES; ++i) {
      if (life_entries[i] != SIZE_MAX) {
        lifecycle(mux, &mux->panes[i], events[life_entries[i]]);
      }
      if (output_entries[i] != SIZE_MAX && (events[output_entries[i]] & WAIT_ERROR)) {
        return CALL_ENDPOINT_CLOSED;
      }
    }
  }
  return mux->error;
}

int main(int argc, char **argv)
{
  struct mux mux = {
    .terminal = {startup_resource("input"), startup_resource("output")},
    .clock = startup_resource("clock"), .tab_width = 8,
    .pointer = startup_resource("terminal_pointer"),
    .clipboard_local = startup_resource("clipboard_local"),
    .clipboard_shared = startup_resource("clipboard_shared"),
  };
  if (argc == 3 && !strcmp(argv[1], "--tab-width")) {
    char *end;
    unsigned long width = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || width < 1 || width > 32) {
      fputs("mux: tab width must be 1..32\n", stderr);
      return 1;
    }
    mux.tab_width = (unsigned)width;
  } else if (argc != 1) {
    fputs("usage: mux [--tab-width 1..32]\n", stderr);
    return 1;
  }
  if (mux.terminal.input == HANDLE_INVALID || mux.terminal.output == HANDLE_INVALID ||
      mux.clock == HANDLE_INVALID || startup_resource("terminal") == HANDLE_INVALID ||
      startup_resource("launcher") == HANDLE_INVALID) {
    fputs("mux: missing terminal, clock, session creation or launcher authority\n", stderr);
    return 1;
  }
  enum call_status status = term_geometry(&mux.terminal, &mux.geometry);
  if (status != CALL_OK || !mux.geometry.columns || !mux.geometry.rows) {
    fprintf(stderr, "mux: cannot query outer geometry (status %u)\n", status);
    return 1;
  }
  if (mux.pointer != HANDLE_INVALID) {
    status = terminal_pointer_acquire(mux.pointer);
    if (status == CALL_OK) {
      mux.pointer_owned = true;
      status = terminal_pointer_get_geometry(mux.pointer, &mux.pointer_geometry);
      if (status == CALL_OK && (!mux.pointer_geometry.cell_width ||
          !mux.pointer_geometry.cell_height || mux.pointer_geometry.cell_width > INT64_MAX ||
          mux.pointer_geometry.cell_height > INT64_MAX)) {
        status = CALL_BAD_REQUEST;
      }
    }
    if (status != CALL_OK) {
      if (mux.pointer_owned) {
        terminal_pointer_release(mux.pointer);
      }
      fprintf(stderr, "mux: cannot acquire terminal pointer (status %u)\n", (unsigned)status);
      return 1;
    }
  }
  mux_layout_init(&mux.layout, 0);
  status = place(&mux);
  struct mux_rect initial = mux.rectangles[0];
  if (!initial.width) {
    initial.width = 1;
  }
  if (!initial.height) {
    initial.height = 1;
  }
  if (status != CALL_OK || !start_pane(&mux, 0, initial)) {
    if (mux.pointer_owned) {
      terminal_pointer_release(mux.pointer);
    }
    fprintf(stderr, "mux: %s\n", mux.notice);
    return 1;
  }
  handle_t passthrough = HANDLE_INVALID;
  status = term_passthrough(&mux.terminal, &passthrough);
  if (status == CALL_OK) {
    status = run(&mux);
    handle_close(passthrough);
  }
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    if (mux.panes[i].used) {
      mux_session_release(&mux.panes[i].session);
      mux_emulator_destroy(&mux.panes[i].emulator);
    }
  }
  free(mux.frame);
  free(mux.previous);
  if (mux.pointer_owned) {
    enum call_status released = terminal_pointer_release(mux.pointer);
    if (status == CALL_OK) {
      status = released;
    }
  }
  term_reset_style(&mux.terminal);
  term_cursor_visible(&mux.terminal, true);
  term_fresh_line(&mux.terminal);
  if (status != CALL_OK) {
    fprintf(stderr, "mux: stopped (status %u)\n", status);
  }
  return status == CALL_OK ? 0 : 1;
}

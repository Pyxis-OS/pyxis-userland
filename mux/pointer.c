#include "mux.h"

#define MUX_WHEEL_ROWS 3
#define MUX_DRAIN_EVENTS 8

void mux_pointer_clear_selection(struct mux *mux)
{
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    if (mux->panes[i].used) {
      mux_emulator_clear_selection(&mux->panes[i].emulator);
    }
  }
  mux->dragging = false;
  mux->pointer_buttons = 0;
  mux->dirty = true;
}

enum call_status mux_pointer_advance_view(struct mux *mux)
{
  if (!mux->pointer_owned) {
    return CALL_OK;
  }
  for (;;) {
    struct terminal_pointer_geometry geometry;
    enum call_status status = terminal_pointer_view_changed(mux->pointer,
        mux->pointer_geometry.surface.generation,
        mux->pointer_geometry.surface.mapping_identity, &geometry);
    if (status == CALL_OK) {
      mux->pointer_geometry = geometry;
      return CALL_OK;
    }
    if (status != CALL_BUSY) {
      return status;
    }
    status = terminal_pointer_get_geometry(mux->pointer, &mux->pointer_geometry);
    if (status != CALL_OK) {
      return status;
    }
  }
}

enum call_status mux_pointer_change_view(struct mux *mux)
{
  mux_pointer_clear_selection(mux);
  return mux_pointer_advance_view(mux);
}

static int hit_pane(const struct mux *mux, int64_t column, int64_t row, bool content)
{
  struct mux_rect view = mux_viewport(mux);
  if (column < 0 || row < 0 || (uint64_t)column >= view.width ||
      (uint64_t)row + 1 >= view.height) {
    return -1;
  }
  for (unsigned i = 0; i < MUX_PANES; ++i) {
    const struct mux_rect *rect = &mux->rectangles[i];
    if (mux->panes[i].used && rect->width && rect->height &&
        (uint64_t)column >= rect->x && (uint64_t)column < rect->x + rect->width &&
        (uint64_t)row >= rect->y + (content ? 1 : 0) &&
        (uint64_t)row < rect->y + rect->height) {
      return (int)i;
    }
  }
  return -1;
}

static bool select_at(struct mux *mux, int64_t column, int64_t row, bool extend,
    bool activate)
{
  struct mux_pane *pane = &mux->panes[mux->selection_pane];
  const struct mux_rect *rect = &mux->rectangles[mux->selection_pane];
  if (!rect->width || rect->height < 2) {
    return false;
  }
  int64_t x = column - (int64_t)rect->x;
  int64_t y = row - (int64_t)rect->y - 1;
  if (x < 0) {
    x = 0;
  } else if ((uint64_t)x >= rect->width) {
    x = (int64_t)rect->width - 1;
  }
  if (y < 0) {
    y = 0;
  } else if ((uint64_t)y >= rect->height - 1) {
    y = (int64_t)rect->height - 2;
  }
  size_t width;
  if (!mux_emulator_row(&pane->emulator, pane->scrollback, (size_t)y, &width) || !width) {
    return false;
  }
  if ((uint64_t)x >= width) {
    if (!extend) {
      return false;
    }
    x = (int64_t)width - 1;
  }
  return mux_emulator_select(&pane->emulator, pane->scrollback, (size_t)y, (size_t)x,
      rect->width, extend, activate);
}

static void pointer_input(struct mux *mux, const struct pointer_event *event)
{
  if (event->type == TERMINAL_POINTER_CLIPBOARD_ACTION) {
    mux_clipboard_action(mux, event);
    return;
  }
  if (event->generation != mux->pointer_geometry.surface.generation ||
      event->mapping_identity != mux->pointer_geometry.surface.mapping_identity) {
    mux_pointer_clear_selection(mux);
    mux->error = terminal_pointer_get_geometry(mux->pointer, &mux->pointer_geometry);
    return;
  }
  /* Outer CONSOLE RESIZED is processed by main before using the new grid. */
  if (event->generation != mux->geometry.generation) {
    return;
  }
  if (event->type != POINTER_INPUT || !(event->flags & POINTER_EVENT_FOCUSED)) {
    mux->pointer_buttons = 0;
    if (mux->dragging) {
      mux_emulator_clear_selection(&mux->panes[mux->selection_pane].emulator);
      mux->dragging = false;
      mux->dirty = true;
    }
    return;
  }

  int64_t column = event->x / (int64_t)mux->pointer_geometry.cell_width;
  int64_t row = event->y / (int64_t)mux->pointer_geometry.cell_height;
  bool pressed = (event->buttons & POINTER_BUTTON_LEFT) &&
      !(mux->pointer_buttons & POINTER_BUTTON_LEFT);
  mux->pointer_buttons = event->buttons;
  if (mux->dragging) {
    struct mux_emulator *emulator = &mux->panes[mux->selection_pane].emulator;
    bool held = (event->buttons & POINTER_BUTTON_LEFT) != 0;
    bool moved = event->x < 0 || event->y < 0 ||
        column != mux->drag_column || row != mux->drag_row;
    if (held || emulator->selection.active) {
      if (!select_at(mux, column, row, true, held && moved)) {
        mux->dragging = false;
      }
    } else {
      mux_emulator_clear_selection(emulator);
    }
    if (!held) {
      mux->dragging = false;
    }
    mux->dirty = true;
  } else if (pressed) {
    mux_pointer_clear_selection(mux);
    mux->pointer_buttons = event->buttons;
    int pane = event->x >= 0 && event->y >= 0 ? hit_pane(mux, column, row, false) : -1;
    if (pane >= 0) {
      if (mux->focused != (unsigned)pane) {
        mux->error = mux_pointer_advance_view(mux);
        if (mux->error != CALL_OK) {
          return;
        }
        mux->prefix = mux->confirm = false;
        mux->escape_size = 0;
      }
      mux->focused = (unsigned)pane;
      mux->dirty = true;
      if (hit_pane(mux, column, row, true) >= 0) {
        mux->selection_pane = (unsigned)pane;
        mux->drag_column = column;
        mux->drag_row = row;
        mux->dragging = select_at(mux, column, row, false, false);
      }
    }
  }

  if (event->wheel && event->x >= 0 && event->y >= 0) {
    int slot = hit_pane(mux, column, row, true);
    if (slot < 0) {
      return;
    }
    struct mux_pane *pane = &mux->panes[slot];
    int64_t rows = (int64_t)event->wheel * MUX_WHEEL_ROWS;
    size_t offset = pane->scrollback;
    if (rows < 0) {
      uint64_t step = (uint64_t)-rows;
      size_t room = pane->emulator.history_count - offset;
      offset += step < room ? (size_t)step : room;
    } else {
      offset -= (uint64_t)rows < offset ? (size_t)rows : offset;
    }
    if (offset != pane->scrollback || (!offset && pane->browsing)) {
      mux->error = mux_pointer_change_view(mux);
      if (mux->error == CALL_OK) {
        pane->scrollback = offset;
        pane->browsing = offset != 0;
        mux->dirty = true;
      }
    }
  }
}

void mux_pointer_drain(struct mux *mux)
{
  for (unsigned i = 0; i < MUX_DRAIN_EVENTS && mux->error == CALL_OK; ++i) {
    struct pointer_event event;
    enum call_status status = terminal_pointer_read(mux->pointer, POINTER_READ_POLL, &event);
    if (status == CALL_TIMED_OUT) {
      return;
    }
    if (status != CALL_OK) {
      mux->error = status;
      return;
    }
    pointer_input(mux, &event);
  }
}

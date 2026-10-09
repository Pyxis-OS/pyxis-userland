#include "mux.h"
#include <clipboard.h>
#include <stdio.h>
#include <stdlib.h>

static void report(struct mux *mux, const char *operation, enum call_status status)
{
  if (status == CALL_OK) {
    snprintf(mux->notice, sizeof(mux->notice), "%s", operation);
  } else {
    snprintf(mux->notice, sizeof(mux->notice), "Clipboard refused (status %u)", status);
  }
  mux->dirty = true;
}

void mux_clipboard_action(struct mux *mux, const struct pointer_event *event)
{
  handle_t clipboard = event->clipboard_layer == CLIPBOARD_LAYER_LOCAL ? mux->clipboard_local :
      event->clipboard_layer == CLIPBOARD_LAYER_SHARED ? mux->clipboard_shared : HANDLE_INVALID;
  enum call_status status = CALL_DENIED;
  if (clipboard == HANDLE_INVALID) {
    report(mux, "", status);
    return;
  }
  if (event->generation != mux->pointer_geometry.surface.generation ||
      event->mapping_identity != mux->pointer_geometry.surface.mapping_identity) {
    clipboard_refuse(clipboard, event->action_id, event->generation,
        event->mapping_identity, event->clipboard_operation);
    report(mux, "", CALL_BUSY);
    return;
  }
  if (event->clipboard_operation == CLIPBOARD_PUBLISH) {
    struct mux_pane *pane = &mux->panes[mux->selection_pane];
    char *text = NULL;
    size_t length = 0;
    status = !mux->dragging && pane->used ?
        mux_emulator_copy_selection(&pane->emulator, CLIPBOARD_TEXT_MAX, &text, &length) :
        CALL_NOT_FOUND;
    if (status == CALL_OK) {
      status = clipboard_publish(clipboard, event->action_id, event->generation,
          event->mapping_identity, text, length);
    } else {
      enum call_status refused = clipboard_refuse(clipboard, event->action_id,
          event->generation, event->mapping_identity, event->clipboard_operation);
      if (refused != CALL_OK) {
        status = refused;
      }
    }
    free(text);
    report(mux, "Copied selection", status);
    return;
  }
  if (event->clipboard_operation == CLIPBOARD_PASTE) {
    struct mux_pane *pane = &mux->panes[mux->focused];
    const struct mux_rect *rect = &mux->rectangles[mux->focused];
    bool clean = mux->input_at == mux->input_size && !mux->prefix && !mux->escape_size &&
        !mux->confirm && pane->used && !pane->root_done && !pane->remove_when_done &&
        !pane->browsing && !pane->scrollback && !pane->pending_size &&
        rect->width && rect->height > 1;
    status = CALL_BUSY;
    if (clean) {
      uint64_t transaction;
      status = clipboard_paste(clipboard, event->action_id, event->generation,
          event->mapping_identity, pane->session.attachment, &transaction);
    } else {
      enum call_status refused = clipboard_refuse(clipboard, event->action_id,
          event->generation, event->mapping_identity, event->clipboard_operation);
      if (refused != CALL_OK) {
        status = refused;
      }
    }
    report(mux, "Paste admitted", status);
  }
}

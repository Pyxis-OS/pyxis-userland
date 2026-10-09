#include "mux.h"
#include <clipboard.h>
#include <stdio.h>
#include <stdlib.h>

static void report(struct mux *mux, uint64_t operation, enum call_status status,
    const char *detail)
{
  bool copy = operation == CLIPBOARD_PUBLISH;
  const char *text = detail;
  if (!text) {
    switch (status) {
    case CALL_OK:
      text = copy ? "Copied selection" : "Paste admitted";
      break;
    case CALL_BUSY:
    case CALL_WOULD_BLOCK:
      text = copy ? "Copy busy; settle the view, then press Copy again" :
          "Paste busy; settle input/view, then press Paste again";
      break;
    case CALL_NOT_FOUND:
      text = copy ? "Copy needs a completed selection; select text first" :
          "Clipboard is empty; copy text before Paste";
      break;
    case CALL_DENIED:
      text = "Clipboard access denied; this layer/action is not authorized";
      break;
    case CALL_LIMIT:
    case CALL_NO_SPACE:
    case CALL_QUOTA:
      text = "Clipboard limit reached; try smaller text or a fresh gesture";
      break;
    case CALL_NO_MEMORY:
      text = "Clipboard memory unavailable; try a fresh gesture later";
      break;
    case CALL_ENDPOINT_CLOSED:
      text = copy ? "Copy endpoint closed; return to a live terminal" :
          "Paste endpoint closed; return to a live prompt";
      break;
    case CALL_WRONG_TYPE:
    case CALL_BAD_OPERATION:
      text = copy ? "Copy unsupported for this terminal" :
          "Paste unsupported; use a stock line reader";
      break;
    case CALL_TIMED_OUT:
      text = "Clipboard action expired; use a fresh gesture";
      break;
    default:
      text = "Clipboard request refused; use a fresh gesture";
      break;
    }
  }
  snprintf(mux->notice, sizeof(mux->notice), "%s", text);
  mux->dirty = true;
}

void mux_clipboard_action(struct mux *mux, const struct pointer_event *event)
{
  handle_t clipboard = event->clipboard_layer == CLIPBOARD_LAYER_LOCAL ? mux->clipboard_local :
      event->clipboard_layer == CLIPBOARD_LAYER_SHARED ? mux->clipboard_shared : HANDLE_INVALID;
  enum call_status status = CALL_DENIED;
  if (clipboard == HANDLE_INVALID) {
    enum call_status refused = terminal_pointer_clipboard_refuse(mux->pointer,
        event->action_id, event->generation, event->mapping_identity,
        event->clipboard_operation);
    if (refused != CALL_OK) {
      status = refused;
    }
    report(mux, event->clipboard_operation, status, NULL);
    return;
  }
  if (event->generation != mux->pointer_geometry.surface.generation ||
      event->mapping_identity != mux->pointer_geometry.surface.mapping_identity) {
    clipboard_refuse(clipboard, event->action_id, event->generation,
        event->mapping_identity, event->clipboard_operation);
    report(mux, event->clipboard_operation, CALL_BUSY, NULL);
    return;
  }
  if (event->clipboard_operation == CLIPBOARD_PUBLISH) {
    struct mux_pane *pane = &mux->panes[mux->selection_pane];
    char *text = NULL;
    size_t length = 0;
    status = !mux->dragging && pane->used ?
        mux_emulator_copy_selection(&pane->emulator, CLIPBOARD_TEXT_MAX, &text, &length) :
        CALL_NOT_FOUND;
    const char *detail = status == CALL_BAD_REQUEST ?
        "Copy refused: selection contains non-ASCII text" : NULL;
    if (status == CALL_OK) {
      status = clipboard_publish(clipboard, event->action_id, event->generation,
          event->mapping_identity, text, length);
    } else {
      enum call_status refused = clipboard_refuse(clipboard, event->action_id,
          event->generation, event->mapping_identity, event->clipboard_operation);
      if (refused != CALL_OK) {
        status = refused;
        detail = NULL;
      }
    }
    free(text);
    report(mux, event->clipboard_operation, status, detail);
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
    report(mux, event->clipboard_operation, status, NULL);
  }
}

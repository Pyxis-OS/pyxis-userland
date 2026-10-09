#ifndef USERSPACE_TERMINAL_POINTER_H
#define USERSPACE_TERMINAL_POINTER_H

#include <abi/terminal_pointer.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* CONTROL authority for this local outer terminal. Acquisition is process
 * owned; release or exit restores kernel handling. Never delegate to panes. */
enum call_status terminal_pointer_acquire(handle_t pointer);
enum call_status terminal_pointer_release(handle_t pointer);
enum call_status terminal_pointer_read(handle_t pointer, uint64_t flags,
    struct pointer_event *event);
enum call_status terminal_pointer_get_geometry(handle_t pointer,
    struct terminal_pointer_geometry *geometry);
/* Validate the current view before a local layout/history change. Success
 * discards queued input/drag and returns the new identity. Clears on failure. */
enum call_status terminal_pointer_view_changed(handle_t pointer, uint64_t generation,
    uint64_t mapping_identity, struct terminal_pointer_geometry *geometry);
enum call_status terminal_pointer_set_image(handle_t pointer, const void *pixels,
    uint32_t width, uint32_t height, uint32_t hotspot_x, uint32_t hotspot_y);
enum call_status terminal_pointer_set_visible(handle_t pointer, bool visible);
enum call_status terminal_pointer_default_image(handle_t pointer);
enum call_status terminal_pointer_state(handle_t pointer, uint64_t *flags);
/* UI control may refuse its native action when startup withheld the chosen
 * clipboard layer. This consumes the attempt without any store authority. */
enum call_status terminal_pointer_clipboard_refuse(handle_t pointer, uint64_t action_id,
    uint64_t generation, uint64_t mapping_identity, uint64_t operation);
/* Cancel Paste and unused clipboard intent for a focus change. The acquired
 * controller retains its pointer queue, accepted buttons and view identity. */
enum call_status terminal_pointer_cancel_clipboard(handle_t pointer);

#endif

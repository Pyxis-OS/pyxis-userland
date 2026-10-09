#ifndef USERSPACE_DISPLAY_H
#define USERSPACE_DISPLAY_H

#include <abi/display.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Acquire the space's single graphics session through a DRAW grant. Output is
 * cleared on failure. The slots belong to the process, not this handle;
 * closing the handle does not release them. Copying grants does not copy session
 * ownership. See abi/display.h for pixel layout and the frame handoff. */
enum call_status display_acquire(handle_t display, struct display_buffer *buffer);
/* Hand over the finished frame in slot; reply names the slot to render next.
 * Do not write slot again until a later reply names it. Cleared on failure. */
enum call_status display_submit(handle_t display, uint64_t slot,
    struct display_submit_reply *reply);
/* Unmaps the acquired slots; do not access their addresses after success.
 * Process exit also releases it and restores the space's TTY. */
enum call_status display_release(handle_t display);
/* Query current destination pixels and generation without acquiring graphics.
 * Existing acquired mappings retain their original layout. Cleared on failure. */
enum call_status display_size(handle_t display, struct display_size_reply *size);
/* Replace the owned slots at this geometry generation. Failure leaves buffer
 * and its slots intact; success invalidates every pointer into the old slots
 * and holds every new slot, so render slot 0 next. */
enum call_status display_replace(handle_t display, uint64_t generation,
    struct display_buffer *buffer);

static inline uintptr_t display_slot_address(const struct display_buffer *buffer, uint64_t slot)
{
  return (uintptr_t)(buffer->address + slot * buffer->size);
}

#endif

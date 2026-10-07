#ifndef USERSPACE_DISPLAY_H
#define USERSPACE_DISPLAY_H

#include <abi/display.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Acquire the space's single graphics session through a DRAW grant. Output is
 * cleared on failure. The mapping belongs to the process, not this handle;
 * closing the handle does not release it. Copying grants does not copy session
 * ownership. See abi/display.h for pixel layout and presentation semantics. */
enum call_status display_acquire(handle_t display, struct display_buffer *buffer);
enum call_status display_present(handle_t display);
/* Unmaps the acquired buffer; do not access its address after success.
 * Process exit also releases it and restores the space's TTY. */
enum call_status display_release(handle_t display);
/* Query current destination pixels and generation without acquiring graphics.
 * Existing acquired mappings retain their original layout. Cleared on failure. */
enum call_status display_size(handle_t display, struct display_size_reply *size);
/* Replace the owned mapping at this geometry generation. Failure leaves buffer
 * and its mapping intact; success invalidates every pointer into the old mapping. */
enum call_status display_replace(handle_t display, uint64_t generation,
    struct display_buffer *buffer);

#endif

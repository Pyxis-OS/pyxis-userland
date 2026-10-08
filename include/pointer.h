#ifndef USERSPACE_POINTER_H
#define USERSPACE_POINTER_H

#include <abi/pointer.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* INPUT authority and ownership of the space's graphics session. The session
 * belongs to the process until release, display release or exit. Handle copies
 * and closure do not transfer or release it; keyboard ownership is independent. */
enum call_status pointer_acquire(handle_t pointer);
enum call_status pointer_release(handle_t pointer);
/* flags is zero to block, POINTER_READ_POLL to poll. Empty polling returns
 * TIMED_OUT. Clears event on failure. See abi/pointer.h for focus/reset rules;
 * callers must release all held buttons on focus changes and state resets. */
enum call_status pointer_read(handle_t pointer, uint64_t flags, struct pointer_event *event);
/* Destination and fixed mapping extents with the identities needed by WARP.
 * Clears geometry on failure. */
enum call_status pointer_geometry(handle_t pointer, struct pointer_geometry *geometry);
/* Copies tightly packed BGRA8 straight-alpha pixels before returning. A failed
 * change preserves the old image. Hotspot must lie inside the image. */
enum call_status pointer_set_image(handle_t pointer, const void *pixels,
    uint32_t width, uint32_t height, uint32_t hotspot_x, uint32_t hotspot_y);
enum call_status pointer_default_image(handle_t pointer);
enum call_status pointer_set_visible(handle_t pointer, bool visible);
/* Coordinates are signed surface-local pixels. Both geometry identities must
 * still match, and the target must lie inside mapping and destination. */
enum call_status pointer_warp(handle_t pointer, int64_t x, int64_t y,
    uint64_t generation, uint64_t mapping_identity);

#endif

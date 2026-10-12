#ifndef USERSPACE_STREAM_H
#define USERSPACE_STREAM_H

#include <abi/stream.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Forward-only native response reader. No retry after a delivered transport
 * failure. A validated zero count is EOF; failure always leaves count zero.
 * Copies share consumption. Hold its separate observation for final framing. */
enum call_status stream_read(handle_t stream, void *bytes, size_t capacity,
    uint64_t deadline_ns, size_t *read);

#endif

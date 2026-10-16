#ifndef USERSPACE_STREAM_H
#define USERSPACE_STREAM_H

#include <abi/stream.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Forward-only native response reader. No retry after a delivered transport
 * failure. Positive capacity is required. A validated zero count is EOF;
 * failure always leaves count zero.
 * Copies share consumption. Generic describe reports the final outcome. */
enum call_status stream_read(handle_t stream, void *bytes, size_t capacity,
    uint64_t deadline_ns, size_t *read);

#endif

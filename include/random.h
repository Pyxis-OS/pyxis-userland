#ifndef USERSPACE_RANDOM_H
#define USERSPACE_RANDOM_H

#include <abi/random.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrowed READ grant. Fill 0..RANDOM_MAX_BYTES or leave bytes untouched on
 * failure. Deadline is absolute monotonic time, at most RANDOM_MAX_WAIT_NS
 * ahead. No partial result, hidden source selection or predictable fallback. */
enum call_status random_read(handle_t random, void *bytes, size_t length, uint64_t deadline_ns);

#endif

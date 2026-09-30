#ifndef USERSPACE_WAIT_H
#define USERSPACE_WAIT_H

#include <abi/syscall.h>
#include <abi/wait.h>
#include <stddef.h>

/* Observe current readiness without reserving progress. Ordinary interests
 * also report relevant closure/errors under the same operation authority.
 * Deadline zero polls; readiness precedes timeout. Output is untouched on
 * failure. No registrations survive return; see abi/wait.h for TCP semantics. */
enum call_status wait_many(const struct wait_interest *interests, size_t count,
    uint64_t deadline_ns, uint64_t *events);

#endif

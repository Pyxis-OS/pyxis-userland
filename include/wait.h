#ifndef USERSPACE_WAIT_H
#define USERSPACE_WAIT_H

#include <abi/syscall.h>
#include <abi/wait.h>
#include <abi/readiness_set.h>
#include <abi/limit.h>
#include <stddef.h>

/* Observe current readiness without reserving progress. Ordinary interests
 * also report relevant closure/errors under the same operation authority.
 * Deadline zero polls; readiness precedes timeout. Output is untouched on
 * failure. Process and group observers use WAIT_COMPLETE with their WAIT authority and
 * may share a list with TCP, terminal attachment and armed console interests.
 * Owned endpoint receivers use READABLE with RECEIVE transport authority;
 * CALL, SEND, CANCEL and RETIRE readiness reserves no delivery. A queued CALL
 * can expire before the ordinary blocking RECEIVE. No registrations
 * survive return; see abi/wait.h for supported targets and event semantics. */
enum call_status wait_many(const struct wait_interest *interests, size_t count,
    uint64_t deadline_ns, uint64_t *events);

/* Owner-local observations. Registration retains observation storage, not a
 * logical I/O grant; close/remove before releasing caller-owned state. Event
 * batches are bounded independently of the number of registrations. */
enum call_status readiness_set_create(handle_t *set, struct call_limit *limit);
enum call_status readiness_set_register(handle_t set, handle_t target,
    uint64_t events, uint64_t tag, struct readiness_set_registration *registration,
    struct call_limit *limit);
enum call_status readiness_set_update(handle_t set,
    struct readiness_set_registration *registration, uint64_t events,
    struct call_limit *limit);
enum call_status readiness_set_remove(handle_t set,
    struct readiness_set_registration registration);
enum call_status readiness_set_wait(handle_t set, uint64_t deadline,
    struct readiness_set_event *events, size_t capacity, size_t *count);

#endif

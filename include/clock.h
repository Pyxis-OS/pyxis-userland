#ifndef USERSPACE_CLOCK_H
#define USERSPACE_CLOCK_H

#include <abi/clock.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* READ authority. Clears the output on failure. See abi/clock.h for the
 * shared monotonic epoch, units, and VM pause/suspend semantics. */
enum call_status clock_now(handle_t clock, uint64_t *nanoseconds);
/* READ authority. UTC Unix time; clears output on failure and validates the
 * normalized nanosecond fraction. No local-time conversion or setting. */
enum call_status clock_wall_now(handle_t clock, struct clock_wall_reading *reading);
/* SLEEP authority. A past deadline succeeds immediately. */
enum call_status clock_sleep_until(handle_t clock, uint64_t deadline_ns);
/* Requires READ and SLEEP. Returns CALL_LIMIT if now + duration overflows;
 * duration zero still checks the supplied capability's rights. */
enum call_status clock_sleep_for(handle_t clock, uint64_t duration_ns);

#endif

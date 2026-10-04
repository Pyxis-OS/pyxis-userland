#ifndef USERSPACE_POINTER_H
#define USERSPACE_POINTER_H

#include <abi/pointer.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* INPUT authority. The session belongs to the process until release or exit,
 * independent of handle copies/closes and of keyboard and display ownership. */
enum call_status pointer_acquire(handle_t pointer);
enum call_status pointer_release(handle_t pointer);
/* flags is zero to block, POINTER_READ_POLL to poll. Empty polling returns
 * TIMED_OUT. Clears event on failure. See abi/pointer.h for focus/reset rules;
 * callers must release all held buttons on focus changes and state resets. */
enum call_status pointer_read(handle_t pointer, uint64_t flags, struct pointer_event *event);

#endif

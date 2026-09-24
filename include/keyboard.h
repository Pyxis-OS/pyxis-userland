#ifndef USERSPACE_KEYBOARD_H
#define USERSPACE_KEYBOARD_H

#include <abi/keyboard.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* INPUT authority. The session belongs to the process until release or exit,
 * independent of handle copies/closes and of display ownership. */
enum call_status keyboard_acquire(handle_t keyboard);
enum call_status keyboard_release(handle_t keyboard);
/* flags is zero to block, KEYBOARD_READ_POLL to poll. Empty polling returns
 * TIMED_OUT. Clears event on failure. See abi/keyboard.h for focus/reset rules;
 * callers must discard all held keys on focus changes and input resets. */
enum call_status keyboard_read(handle_t keyboard, uint64_t flags,
                              struct keyboard_event *event);

#endif

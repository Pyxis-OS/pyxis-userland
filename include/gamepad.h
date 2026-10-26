#ifndef USERSPACE_GAMEPAD_H
#define USERSPACE_GAMEPAD_H

#include <abi/gamepad.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* INPUT authority in this space. Acquisition is exclusive and process-owned;
 * copies and closes do not release it. An empty catalog still permits acquire. */
enum call_status gamepad_acquire(handle_t gamepad);
enum call_status gamepad_release(handle_t gamepad);
/* Zero blocks; GAMEPAD_READ_POLL returns TIMED_OUT when empty. Outputs are
 * cleared on failure. RESET invalidates all holds and requires catalog/state
 * recovery. Never consume state without VALID; see ABI focus/rearming rules. */
enum call_status gamepad_read(handle_t gamepad, uint64_t flags,
    struct gamepad_event *event);
/* Repeat enumeration when generation changes while collecting INFO/STATE.
 * An attachment instance never identifies a later replug of the same device. */
enum call_status gamepad_catalog(handle_t gamepad, struct gamepad_catalog *catalog);
enum call_status gamepad_info(handle_t gamepad, uint64_t instance,
    struct gamepad_info *info);
enum call_status gamepad_state(handle_t gamepad, uint64_t instance,
    struct gamepad_event *state);

#endif

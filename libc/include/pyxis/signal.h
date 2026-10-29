#ifndef LIBC_PYXIS_SIGNAL_H
#define LIBC_PYXIS_SIGNAL_H

#include <abi/wait.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pyxis_signal_wait {
  struct wait_interest interests[2];
  size_t count;
  bool pending;
};

/* Initial-thread-only cooperative loop bridge. Both calls return 0 on success
 * while preserving errno, or -1 with errno set; workers return ENOTSUP.
 * Prepare collects without invoking handlers; pending means an eligible caught
 * or default-action event, so the loop must not block. Output is unchanged on
 * failure (including EINVAL for NULL).
 *
 * Interests borrow observation-only native handles until the next checkpoint
 * or intervening signal/descriptor mutation. Use raw SDK operations between
 * prepare and checkpoint. Withdraw temporary signal registrations/references
 * before checkpoint or any application callback; no grant ownership transfers. */
int pyxis_signal_wait_prepare(struct pyxis_signal_wait *wait);

/* At a clean application boundary, collect and dispatch one bounded libc
 * snapshot under the effective signal mask. Handlers may call application code;
 * the caller must first release all operation-local resources and locks. Fails
 * with EBUSY inside an inhibited libc operation; never overrides its admission.
 * Success reports completion, not a count of handlers; it may dispatch none. */
int pyxis_signal_checkpoint(void);

/* Optional explicit WINCH terminal selection, overriding automatic stdio
 * selection on handler installation. Initial-thread-only. Captures this fd's
 * exact slot generation and SIZE baseline; close/replacement ends its binding.
 * No native grant is copied, no raw mode is changed and no input is consumed.
 * Failure preserves the previous selection. */
int pyxis_signal_set_terminal(int descriptor);

#ifdef __cplusplus
}
#endif

#endif

#ifndef LIBC_SIGNAL_INTERNAL_H
#define LIBC_SIGNAL_INTERNAL_H

#include <abi/wait.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

struct libc_signal_wait {
  struct wait_interest interests[2];
  size_t count;
  bool pending;
};

struct libc_signal_delivery {
  bool caught;
  bool restart;
};

/* Collect native state without callbacks. Pending means an eligible caught or
 * default-action event. Interests omit already-latched masked/ignored levels.
 * Worker calls return an empty observation. Failure sets errno. */
int libc_signal_prepare(struct libc_signal_wait *wait);

/* Enter admits signal observation while inhibiting dispatch. Only audited
 * outer operations with a clean return checkpoint may enter. Leave never
 * dispatches: release all references, locks and registrations first. */
void libc_signal_enter(void);
void libc_signal_leave(void);
bool libc_signal_admitted(void);

/* At depth zero collect and dispatch one bounded pending snapshot, preserving
 * errno. Restart is true only when at least one handler ran and every caught
 * action selected SA_RESTART. Each caller still owns its restart whitelist,
 * progress, original deadline and exact-identity check. Nested clean checkpoints
 * follow the effective mask, including SA_NODEFER. */
struct libc_signal_delivery libc_signal_checkpoint(void);

/* No callbacks. NULL keeps the current mask; non-NULL is initial-thread-only.
 * Save on success, restore on every completed temporary-wait path. Dispatch
 * made eligible only by restoration cannot change the completed wait result. */
int libc_signal_mask_install(const sigset_t *temporary, sigset_t *previous);
void libc_signal_mask_restore(const sigset_t *previous);

/* Initial-thread retirement only; workers are a no-op. Release observation
 * storage and libc's own receiver without migrating delivery. */
void libc_signal_retire(void);

#endif

#ifndef LIBC_SIGNAL_SCOPE_H
#define LIBC_SIGNAL_SCOPE_H

#include "signal_internal.h"

struct libc_signal_scope {
  unsigned reserved;
};

static inline struct libc_signal_scope libc_signal_scope_begin(void)
{
  libc_signal_enter();
  return (struct libc_signal_scope){0};
}

/* Declare first, before operation-local cleanup owners: reverse C cleanup
 * order releases every owner before this scope. libc_signal_leave only removes
 * inhibition; the separate checkpoint is safe after those owners are gone.
 * Nested scopes remain inhibited by their outer caller. */
static inline void libc_signal_scope_end(struct libc_signal_scope *scope)
{
  (void)scope;
  libc_signal_leave();
  libc_signal_checkpoint();
}

#endif

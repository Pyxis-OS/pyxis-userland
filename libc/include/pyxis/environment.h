#ifndef LIBC_PYXIS_ENVIRONMENT_H
#define LIBC_PYXIS_ENVIRONMENT_H

#include <abi/startup.h>
#include <abi/syscall.h>
#include <stddef.h>

struct pyxis_environment_snapshot {
  struct startup_variable *variables;
  size_t count;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow a current value. Absence is CALL_NOT_FOUND; an empty value is CALL_OK.
 * Other failures preserve their native status, so configuration callers cannot
 * mistake failed initialization for an absent setting. Mutation may invalidate
 * the result; do not modify or free it. Failure clears *value. */
enum call_status pyxis_environment_get(const char *name, const char **value);

/* Own a copy of the current single-threaded libc environment, suitable for a
 * native launch. Each name/value address points to independently owned storage.
 * Mutation does not invalidate the copy. An empty snapshot has NULL variables.
 * Initialize an uninitialized/closed record; close a live copy before reuse.
 * Failure clears the record and preserves the process environment. */
enum call_status pyxis_environment_snapshot_init(struct pyxis_environment_snapshot *snapshot);

/* Release owned names, values and array, then clear the record. NULL and a
 * cleared record are safe; do not edit its pointers/count or close a copied
 * record twice. */
void pyxis_environment_snapshot_close(struct pyxis_environment_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif

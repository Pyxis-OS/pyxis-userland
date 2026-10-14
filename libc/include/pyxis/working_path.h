#ifndef LIBC_PYXIS_WORKING_PATH_H
#define LIBC_PYXIS_WORKING_PATH_H

#include <path.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One retained native cwd, seeded lazily from startup. The view address stays
 * stable, but its contents and borrowed description change on successful cd.
 * Exclude change and clear from lookup through the last use of a
 * borrowed view. Internal file lookup uses owned snapshots instead. Unknown
 * initial spelling does not prevent relative capability lookup. */
enum call_status pyxis_working_context(const struct path_context **context);
const char *pyxis_working_path(void);
enum call_status pyxis_working_change(const char *path);

/* Release the cwd and description once, then keep an initialized empty state.
 * Session bootstrap teardown must never resurrect its closed startup chain. */
void pyxis_working_clear(void);

/* Owns the directory chain and description.
 * Fresh output only, never shallow-copy. NULL override snapshots current cwd;
 * an explicit override resolves in isolation. Parent mutation preserves this
 * chain/path. Close once; uncertain native releases are never retried. */
struct pyxis_working_snapshot {
  struct path_context context;
  char *path;
};

enum call_status pyxis_working_snapshot_init(struct pyxis_working_snapshot *snapshot,
    const char *cwd_override);
void pyxis_working_snapshot_close(struct pyxis_working_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif

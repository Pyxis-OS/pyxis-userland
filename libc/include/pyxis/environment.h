#ifndef LIBC_PYXIS_ENVIRONMENT_H
#define LIBC_PYXIS_ENVIRONMENT_H

#include <environment.h>

struct pyxis_environment_snapshot {
  struct environment_capture capture;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow current TEXT. Mutation may invalidate it; exclude native and libc
 * mutation through last use. No capability enters libc's value cache. */
enum call_status pyxis_environment_get(const char *name, const char **value);
enum call_status pyxis_environment_copy(const char *name, char **value);
/* Initialize an unowned TEXT snapshot. Failure leaves it empty; successful
 * values remain owned and immutable until close, independently of mutation.
 * snapshot_init captures exported TEXT only for selected launch profiles;
 * text_snapshot_init includes private TEXT too. Neither retains CAP grants. */
enum call_status pyxis_environment_snapshot_init(struct pyxis_environment_snapshot *snapshot);
enum call_status pyxis_environment_text_snapshot_init(struct pyxis_environment_snapshot *snapshot);
void pyxis_environment_snapshot_close(struct pyxis_environment_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
#endif

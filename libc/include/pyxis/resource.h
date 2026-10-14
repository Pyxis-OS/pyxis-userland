#ifndef LIBC_PYXIS_RESOURCE_H
#define LIBC_PYXIS_RESOURCE_H

#include <resource.h>
#include <stdbool.h>
#include <stdio.h>

struct pyxis_open_result {
  bool available;
  struct resource_result resource;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Use a zero-initialized/closed output. Failure can retain an owned typed failure
 * and its immutable error body; no hidden last-error state or fresh OPEN is used. */
FILE *pyxis_fopen(const char *path, const char *mode, struct pyxis_open_result *result);
int pyxis_open_result_close(struct pyxis_open_result *result);
FILE *pyxis_open_error_body(const struct pyxis_open_result *result);
/* Metadata only; does not move the cursor or return an owned body handle. */
int pyxis_describe(FILE *stream, struct describe_reply *description);
FILE *pyxis_error_body(FILE *stream);
/* Discard queued output and abort a pending transaction before releasing this
 * FILE. Successful intentional abort/release returns zero; other aliases keep
 * the aborted failure. Nonpending streams close normally. */
int pyxis_fclose_abandon(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif

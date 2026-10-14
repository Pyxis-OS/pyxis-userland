#ifndef LIBC_PYXIS_HTTP_H
#define LIBC_PYXIS_HTTP_H

#include <abi/http_transaction.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdio.h>

struct pyxis_http_options {
  const char *headers;
  size_t headers_size;
  uint64_t deadline_ns;
  uint64_t flags;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Preparation performs no network submission. Zero deadline selects one
 * 30-second budget; supplied deadlines are absolute monotonic times. Grants
 * come only from the selected environment entries and clock. Exact-stream flush
 * and final owning close submit through the generic transaction interface. */
FILE *pyxis_http_prepare(const char *uri, const char *mode,
    const struct pyxis_http_options *options);
int pyxis_http_status(FILE *stream, struct http_transaction_status *status);
int pyxis_http_abort(FILE *request);

#ifdef __cplusplus
}
#endif

#endif

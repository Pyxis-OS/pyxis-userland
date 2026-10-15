#ifndef LIBC_LEGACY_HTTP_H
#define LIBC_LEGACY_HTTP_H

#include "descriptor.h"
#include <resource.h>
#include <http_transaction.h>

struct libc_legacy_open {
  struct resource_result result;
  struct http_prepared prepared;
  struct pyxis_response_info response;
  uint64_t deadline;
  bool submitted;
};

/* Isolated URL selection until B8. Never a fallback from typed OPEN. The caller
 * closes opened on every outcome, retaining any typed error body first. */
bool libc_legacy_http_path(const char *path);
enum call_status libc_legacy_http_open(const char *path,
    const struct descriptor_mode *mode, const struct pyxis_http_options *options,
    bool prepare_only, struct libc_legacy_open *opened);
void libc_legacy_http_close(struct libc_legacy_open *opened);

#endif

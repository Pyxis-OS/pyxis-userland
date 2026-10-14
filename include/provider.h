#ifndef USERSPACE_PROVIDER_H
#define USERSPACE_PROVIDER_H

#include <abi/provider.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

struct provider_metadata {
  uint64_t protocol;
  uint64_t representation;
  size_t media_type_size;
  char media_type[PROVIDER_MEDIA_TYPE_MAX_BYTES + 1];
};

struct provider_result {
  enum call_status status;
  uint64_t delivery;
  struct provider_metadata metadata;
};

/* Transitional strict FILE adapter; typed results/describe remain unfinished.
 * OPEN copies opaque target bytes, intent, flags and the original absolute
 * deadline. No HTTP context or fallback. Success owns one exported FILE with
 * exactly the intent's FILE rights and CALL transport. Failure clears file
 * and closes all provisional attachments. Only CALL_OK makes result->status
 * meaningful; delivery survives transport/local validation failure.
 * Metadata is copied and NUL-terminated. No retry or allocation. */
enum call_status provider_open(handle_t provider, const char *target, uint64_t intent,
    uint64_t flags, uint64_t deadline_ns, struct provider_result *result, handle_t *file);

#endif

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
  uint64_t provider_status;
  uint64_t delivery;
  struct provider_metadata metadata;
};

/* OPEN sends the complete URI unchanged and requests exact FILE rights.
 * The return value reports transport or local validation failure. Only CALL_OK
 * makes result->status and provider_status meaningful, including provider errors.
 * delivery retains the endpoint's delivery state even when validation fails.
 * Successful OPEN owns one exported byte-file grant with CALL transport; its
 * metadata copies and NUL-terminates the media type. Other outcomes clear file
 * and metadata and close every returned grant. result and file are required.
 * deadline_ns is absolute monotonic time; zero waits without a deadline.
 * Malformed application replies report BAD_REQUEST. No retry or allocation. */
enum call_status provider_open(handle_t provider, const char *uri, uint64_t rights,
    uint64_t deadline_ns, struct provider_result *result, handle_t *file);

#endif

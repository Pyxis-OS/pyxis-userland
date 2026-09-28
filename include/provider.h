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

/* OPEN sends the complete URI unchanged and requests exact FILE rights.
 * Success owns one exported byte-file grant with CALL transport. Optional
 * metadata describes the representation without granting extra authority.
 * Media-type bytes are copied and NUL-terminated here. Failure clears outputs
 * and closes every returned grant. Transport failures retain their status;
 * malformed application replies report BAD_REQUEST. No retry or allocation. */
enum call_status provider_open(handle_t provider, const char *uri, uint64_t rights,
    struct provider_metadata *metadata, handle_t *file);

#endif

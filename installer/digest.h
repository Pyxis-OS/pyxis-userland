#ifndef USERSPACE_INSTALLER_DIGEST_H
#define USERSPACE_INSTALLER_DIGEST_H

#include <sha256.h>
#include <stdbool.h>
#include <stddef.h>

/* Initialize with {0}. Each caller-owned context has an independent stream.
 * A failed update or finish ends it; no shared initialization is required. */
struct install_digest {
  struct sha256_context context;
  bool active;
};

bool install_digest_begin(struct install_digest *digest);
bool install_digest_update(struct install_digest *digest, const void *bytes, size_t size);
bool install_digest_finish(struct install_digest *digest, char sha256[65]);
void install_digest_abort(struct install_digest *digest);

#endif

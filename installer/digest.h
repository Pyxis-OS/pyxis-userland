#ifndef USERSPACE_INSTALLER_DIGEST_H
#define USERSPACE_INSTALLER_DIGEST_H

#include <psa/crypto.h>
#include <stdbool.h>
#include <stddef.h>

/* Initialize with {0}. Each context owns one stream; callers initialize the
 * shared PSA allocator/entropy with hash_init and close it after all streams. */
struct install_digest {
  psa_hash_operation_t operation;
};

bool install_digest_begin(struct install_digest *digest);
bool install_digest_update(struct install_digest *digest, const void *bytes, size_t size);
bool install_digest_finish(struct install_digest *digest, char sha256[65]);
void install_digest_abort(struct install_digest *digest);

#endif

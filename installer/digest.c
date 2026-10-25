#include "digest.h"

void install_digest_abort(struct install_digest *digest)
{
  *digest = (struct install_digest){0};
}

bool install_digest_begin(struct install_digest *digest)
{
  sha256_init(&digest->context);
  digest->active = true;
  return true;
}

bool install_digest_update(struct install_digest *digest, const void *bytes, size_t size)
{
  if (!digest->active || !sha256_update(&digest->context, bytes, size)) {
    install_digest_abort(digest);
    return false;
  }
  return true;
}

bool install_digest_finish(struct install_digest *digest, char sha256[65])
{
  unsigned char hash[SHA256_DIGEST_BYTES];
  bool ok = digest->active && sha256_finish(&digest->context, hash);
  install_digest_abort(digest);
  if (!ok) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(hash); ++index) {
    sha256[index * 2] = hex[hash[index] >> 4];
    sha256[index * 2 + 1] = hex[hash[index] & 15];
  }
  sha256[64] = '\0';
  return true;
}

#include "digest.h"

void install_digest_abort(struct install_digest *digest)
{
  psa_hash_abort(&digest->operation);
}

bool install_digest_begin(struct install_digest *digest)
{
  install_digest_abort(digest);
  return psa_hash_setup(&digest->operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

bool install_digest_update(struct install_digest *digest, const void *bytes, size_t size)
{
  const unsigned char *input = bytes;
  while (size) {
    size_t count = size > 4096 ? 4096 : size;
    if (psa_hash_update(&digest->operation, input, count) != PSA_SUCCESS) {
      install_digest_abort(digest);
      return false;
    }
    input += count;
    size -= count;
  }
  return true;
}

bool install_digest_finish(struct install_digest *digest, char sha256[65])
{
  unsigned char hash[32];
  size_t length = 0;
  psa_status_t status = psa_hash_finish(&digest->operation, hash, sizeof(hash), &length);
  install_digest_abort(digest);
  if (status != PSA_SUCCESS || length != sizeof(hash)) {
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

#include "hash.h"
#include <mbedtls/platform.h>
#include <clock.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <random.h>
#include <stdlib.h>
#include <string.h>

static handle_t entropy_clock, entropy_random;
static uint64_t entropy_deadline;

/* The configured PSA initialization seeds its RNG even for a hash-only user.
 * Borrow only the named native entropy and monotonic-clock grants. */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
    size_t *estimate_bits, unsigned char *output, size_t output_size)
{
  *estimate_bits = 0;
  if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) {
    return PSA_ERROR_NOT_SUPPORTED;
  }
  size_t offset = 0;
  while (offset < output_size) {
    uint64_t now;
    if (clock_now(entropy_clock, &now) != CALL_OK || now >= entropy_deadline) {
      return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    size_t count = output_size - offset;
    if (count > RANDOM_MAX_BYTES) {
      count = RANDOM_MAX_BYTES;
    }
    if (random_read(entropy_random, output + offset, count, entropy_deadline) != CALL_OK) {
      return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    offset += count;
  }
  *estimate_bits = output_size * 8;
  return PSA_SUCCESS;
}

bool hash_init(handle_t clock, handle_t random)
{
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(2000000000) ||
      random == HANDLE_INVALID) {
    return false;
  }
  entropy_clock = clock;
  entropy_random = random;
  entropy_deadline = now + UINT64_C(2000000000);
  mbedtls_platform_set_calloc_free(calloc, free);
  return psa_crypto_init() == PSA_SUCCESS;
}

void hash_close(void)
{
  mbedtls_psa_crypto_free();
}

bool file_digest(const void *bytes, size_t size, char digest[65])
{
  psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
  unsigned char hash[32];
  size_t length = 0;
  psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
  const unsigned char *input = bytes;
  /* Bound PSA's temporary input copy independently of the file buffer. */
  while (status == PSA_SUCCESS && size) {
    size_t count = size > 4096 ? 4096 : size;
    status = psa_hash_update(&operation, input, count);
    input += count;
    size -= count;
  }
  if (status == PSA_SUCCESS) {
    status = psa_hash_finish(&operation, hash, sizeof(hash), &length);
  }
  psa_hash_abort(&operation);
  if (status != PSA_SUCCESS || length != sizeof(hash)) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(hash); index++) {
    digest[index * 2] = hex[hash[index] >> 4];
    digest[index * 2 + 1] = hex[hash[index] & 15];
  }
  digest[64] = 0;
  return true;
}

static unsigned hex_lower(unsigned byte)
{
  return byte >= 'A' && byte <= 'F' ? byte + 'a' - 'A' : byte;
}

bool digest_valid(const char *digest)
{
  if (!digest || strlen(digest) != 64) {
    return false;
  }
  for (; *digest; digest++) {
    unsigned byte = hex_lower((unsigned char)*digest);
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool digest_equal(const char *expected, const char *actual)
{
  if (!digest_valid(expected) || !digest_valid(actual)) {
    return false;
  }
  for (size_t index = 0; index < 64; index++) {
    if (hex_lower((unsigned char)expected[index]) != hex_lower((unsigned char)actual[index])) {
      return false;
    }
  }
  return true;
}
